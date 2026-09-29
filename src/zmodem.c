/*
 * ZMODEM
 *
 * - ヘッダは ZPAD ('*') ZDLE (0x18) に続けて形式 ('A' バイナリ CRC-16 / 'B' 16 進 / 'C' バイナリ CRC-32)、
 *   種類 1 バイトと 4 バイト (位置なら下位から、フラグなら ZF3〜ZF0 の順)、CRC が続く
 * - データは ZDLE でエスケープし、ZDLE と終わりの種類 (ZCRCE / G / Q / W) と CRC でサブパケットを閉じる
 * - 受信側が ZRINIT で能力を知らせ、送信側は ZFILE、ZRPOS で示された位置から ZDATA を流す。
 *   誤りがあれば受信側が ZRPOS でその位置からやり直させる
 * - CAN (= ZDLE) が 5 つ続いたら中止
 */
#include "xfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ZPAD = '*', ZDLE = 0x18, ZBIN = 'A', ZHEX = 'B', ZBIN32 = 'C' };
enum {
    ZRQINIT, ZRINIT, ZSINIT, ZACK, ZFILE, ZSKIP, ZNAK, ZABORT, ZFIN, ZRPOS,
    ZDATA, ZEOF, ZFERR, ZCRC, ZCHALLENGE, ZCOMPL, ZCAN, ZFREECNT, ZCOMMAND,
};
enum { ZCRCE = 'h', ZCRCG = 'i', ZCRCQ = 'j', ZCRCW = 'k', ZRUB0 = 'l', ZRUB1 = 'm' };
/* ZRINIT の ZF0 */
enum { CANFDX = 0x01, CANOVIO = 0x02, CANFC32 = 0x20, ESCCTL = 0x40 };
enum { ZCBIN = 1 };
/* hdr[] の並び: 位置は hdr[0] が下位、フラグは hdr[3] が ZF0 */
#define ZF0 3

/* 読み取りの結果 (T_NODATA / T_DISCONNECT のほか) */
#define ZM_ERR (-200) /* CRC が合わないなど */
#define ZM_CAN (-201) /* CAN が 5 つ続いた */
#define GOTOR 0x100   /* サブパケットの終わり (下位に種類) */

#define RETRY 10
#define SUBLEN 1024
#define MAXSUB 8192

struct zm {
    struct term *t;
    unsigned char in[4096];
    size_t ipos, ilen;
    bool rx32;  /* 最後に受けたヘッダが CRC-32 (続くデータも CRC-32) */
    bool tx32;  /* こちらが送るバイナリヘッダとデータを CRC-32 にする */
    bool escctl;
};

static unsigned short crc16_up(unsigned short crc, unsigned char c) {
    crc ^= (unsigned short)(c << 8);
    for (int i = 0; i < 8; i++) crc = crc & 0x8000 ? (unsigned short)((crc << 1) ^ 0x1021) : (unsigned short)(crc << 1);
    return crc;
}

static uint32_t crc32_up(uint32_t crc, unsigned char c) {
    crc ^= c;
    for (int i = 0; i < 8; i++) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    return crc;
}

static uint32_t hdr_pos(const unsigned char *h) {
    return (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
}

static void pos_hdr(unsigned char *h, uint32_t pos) {
    h[0] = (unsigned char)pos;
    h[1] = (unsigned char)(pos >> 8);
    h[2] = (unsigned char)(pos >> 16);
    h[3] = (unsigned char)(pos >> 24);
}

/* ------------------------------------------------------------ 読む */

static int zgetc(struct zm *z, int ms) {
    if (z->ipos >= z->ilen) {
        int n = term_read_bin(z->t, z->in, sizeof z->in, ms);
        if (n < 0) return n;
        z->ipos = 0;
        z->ilen = (size_t)n;
    }
    return z->in[z->ipos++];
}

/* 読める入力があるか (待たない) */
static bool zready(struct zm *z) {
    if (z->ipos < z->ilen) return true;
    int n = term_read_bin(z->t, z->in, sizeof z->in, 0);
    if (n <= 0) return false;
    z->ipos = 0;
    z->ilen = (size_t)n;
    return true;
}

static bool is_flow(int c) { return c == 0x11 || c == 0x13 || c == 0x91 || c == 0x93; }

/* ZDLE のエスケープを戻して 1 バイト読む。サブパケットの終わりは GOTOR | 種類 */
static int zdlread(struct zm *z, int ms) {
    int c;
    do {
        if ((c = zgetc(z, ms)) < 0) return c;
    } while (is_flow(c));
    if (c != ZDLE) return c;
    int cans = 1;
    for (;;) {
        if ((c = zgetc(z, ms)) < 0) return c;
        if (is_flow(c)) continue;
        if (c != ZDLE) break;
        if (++cans >= 5) return ZM_CAN;
    }
    if (c >= ZCRCE && c <= ZCRCW) return GOTOR | c;
    if (c == ZRUB0) return 0x7F;
    if (c == ZRUB1) return 0xFF;
    if ((c & 0x60) == 0x40) return c ^ 0x40;
    return ZM_ERR;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hexbyte(struct zm *z, int ms) {
    int v[2];
    for (int i = 0; i < 2; i++) {
        int c;
        do {
            if ((c = zgetc(z, ms)) < 0) return c;
        } while (is_flow(c));
        if ((v[i] = hexval(c & 0x7F)) < 0) return ZM_ERR;
    }
    return v[0] << 4 | v[1];
}

/* ヘッダを待つ。種類 (0 以上) か、T_NODATA / T_DISCONNECT / ZM_ERR / ZM_CAN */
static int zgethdr(struct zm *z, int ms, unsigned char *hdr) {
    int cans = 0;
    for (;;) {
        int c = zgetc(z, ms);
        if (c < 0) return c;
        if (c == ZDLE) {
            if (++cans >= 5) return ZM_CAN;
        } else cans = 0;
        if (c != ZPAD) continue;
        do c = zgetc(z, ms);
        while (c == ZPAD);
        if (c < 0) return c;
        if (c != ZDLE) continue;
        int fmt = zgetc(z, ms);
        if (fmt < 0) return fmt;
        unsigned char b[5];
        if (fmt == ZHEX) {
            unsigned short crc = 0;
            for (int i = 0; i < 5; i++) {
                int v = hexbyte(z, ms);
                if (v < 0) return v;
                b[i] = (unsigned char)v;
                crc = crc16_up(crc, b[i]);
            }
            int h = hexbyte(z, ms), l = h < 0 ? h : hexbyte(z, ms);
            if (h < 0 || l < 0) return h < 0 ? h : l;
            if (crc != (h << 8 | l)) return ZM_ERR;
            z->rx32 = false;
            /* 後ろの CR LF (XON) は次のヘッダを探すときに読み捨てる */
        } else if (fmt == ZBIN || fmt == ZBIN32) {
            bool c32 = fmt == ZBIN32;
            unsigned short crc = 0;
            uint32_t crc32 = 0xFFFFFFFFu;
            for (int i = 0; i < 5; i++) {
                int v = zdlread(z, ms);
                if (v < 0) return v;
                if (v & GOTOR) return ZM_ERR;
                b[i] = (unsigned char)v;
                if (c32) crc32 = crc32_up(crc32, b[i]);
                else crc = crc16_up(crc, b[i]);
            }
            uint32_t got = 0;
            for (int i = 0; i < (c32 ? 4 : 2); i++) {
                int v = zdlread(z, ms);
                if (v < 0) return v;
                if (v & GOTOR) return ZM_ERR;
                got = c32 ? got | (uint32_t)v << (8 * i) : got << 8 | (uint32_t)v;
            }
            if (c32 ? ~crc32 != got : crc != got) return ZM_ERR;
            z->rx32 = c32;
        } else continue;
        memcpy(hdr, b + 1, 4);
        return b[0];
    }
}

/* データのサブパケットを読む。終わりの種類 (ZCRCE など) か負の値 */
static int zrecvdata(struct zm *z, unsigned char *buf, size_t max, size_t *len) {
    size_t n = 0;
    unsigned short crc = 0;
    uint32_t crc32 = 0xFFFFFFFFu;
    for (;;) {
        int c = zdlread(z, 10000);
        if (c < 0) return c;
        bool end = c & GOTOR;
        unsigned char b = (unsigned char)c;
        if (z->rx32) crc32 = crc32_up(crc32, b);
        else crc = crc16_up(crc, b);
        if (end) {
            uint32_t got = 0;
            for (int i = 0; i < (z->rx32 ? 4 : 2); i++) {
                int v = zdlread(z, 10000);
                if (v < 0) return v;
                if (v & GOTOR) return ZM_ERR;
                got = z->rx32 ? got | (uint32_t)v << (8 * i) : got << 8 | (uint32_t)v;
            }
            if (z->rx32 ? ~crc32 != got : crc != got) return ZM_ERR;
            *len = n;
            return b;
        }
        if (n >= max) return ZM_ERR;
        buf[n++] = b;
    }
}

/* ------------------------------------------------------------ 書く */

static const char hexd[] = "0123456789abcdef";

static int zshhdr(struct zm *z, int type, const unsigned char *hdr) {
    unsigned char b[5] = {(unsigned char)type, hdr[0], hdr[1], hdr[2], hdr[3]};
    char s[32];
    int k = 0;
    s[k++] = ZPAD;
    s[k++] = ZPAD;
    s[k++] = ZDLE;
    s[k++] = ZHEX;
    unsigned short crc = 0;
    for (int i = 0; i < 5; i++) {
        s[k++] = hexd[b[i] >> 4];
        s[k++] = hexd[b[i] & 15];
        crc = crc16_up(crc, b[i]);
    }
    s[k++] = hexd[crc >> 12];
    s[k++] = hexd[(crc >> 8) & 15];
    s[k++] = hexd[(crc >> 4) & 15];
    s[k++] = hexd[crc & 15];
    s[k++] = '\r';
    s[k++] = (char)0x8A;
    if (type != ZFIN && type != ZACK) s[k++] = 0x11;
    return term_write_bin(z->t, s, (size_t)k);
}

static void zput(struct zm *z, unsigned char *o, size_t *k, unsigned char c) {
    bool esc;
    switch (c) {
    case ZDLE: case 0x10: case 0x90: case 0x11: case 0x91: case 0x13: case 0x93: case 0x0D: case 0x8D:
        esc = true;
        break;
    default:
        esc = z->escctl && (c & 0x60) == 0;
    }
    if (esc) {
        o[(*k)++] = ZDLE;
        o[(*k)++] = c ^ 0x40;
    } else o[(*k)++] = c;
}

static int zsbhdr(struct zm *z, int type, const unsigned char *hdr) {
    unsigned char b[5] = {(unsigned char)type, hdr[0], hdr[1], hdr[2], hdr[3]}, o[32];
    size_t k = 0;
    o[k++] = ZPAD;
    o[k++] = ZDLE;
    o[k++] = z->tx32 ? ZBIN32 : ZBIN;
    unsigned short crc = 0;
    uint32_t crc32 = 0xFFFFFFFFu;
    for (int i = 0; i < 5; i++) {
        zput(z, o, &k, b[i]);
        if (z->tx32) crc32 = crc32_up(crc32, b[i]);
        else crc = crc16_up(crc, b[i]);
    }
    if (z->tx32) {
        crc32 = ~crc32;
        for (int i = 0; i < 4; i++) zput(z, o, &k, (unsigned char)(crc32 >> (8 * i)));
    } else {
        zput(z, o, &k, (unsigned char)(crc >> 8));
        zput(z, o, &k, (unsigned char)crc);
    }
    return term_write_bin(z->t, o, k);
}

static int zsdata(struct zm *z, const unsigned char *data, size_t len, int end) {
    unsigned char o[SUBLEN * 2 + 16];
    size_t k = 0;
    unsigned short crc = 0;
    uint32_t crc32 = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        zput(z, o, &k, data[i]);
        if (z->tx32) crc32 = crc32_up(crc32, data[i]);
        else crc = crc16_up(crc, data[i]);
    }
    o[k++] = ZDLE;
    o[k++] = (unsigned char)end;
    if (z->tx32) {
        crc32 = ~crc32_up(crc32, (unsigned char)end);
        for (int i = 0; i < 4; i++) zput(z, o, &k, (unsigned char)(crc32 >> (8 * i)));
    } else {
        crc = crc16_up(crc, (unsigned char)end);
        zput(z, o, &k, (unsigned char)(crc >> 8));
        zput(z, o, &k, (unsigned char)crc);
    }
    if (end == ZCRCW) o[k++] = 0x11;
    return term_write_bin(z->t, o, k);
}

static int send_pos(struct zm *z, int type, uint32_t pos) {
    unsigned char h[4];
    pos_hdr(h, pos);
    return zshhdr(z, type, h);
}

/* ------------------------------------------------------------ 受信 */

int zm_recv(struct term *t, char *name, size_t namesz, unsigned char **data, size_t *len) {
    struct zm z = {.t = t};
    static const unsigned char rinit[4] = {0, 0, 0, CANFDX | CANOVIO | CANFC32};
    unsigned char hdr[4], *buf = NULL, *blk = malloc(MAXSUB);
    size_t cap = 0, off = 0, want = 0;
    bool infile = false, got_file = false, badname = false;
    int errors = 0, result = XF_FAIL;

    if (zshhdr(&z, ZRINIT, rinit) < 0) goto disc;
    for (;;) {
        int type = zgethdr(&z, 10000, hdr);
        if (type == T_DISCONNECT) goto disc;
        if (type == ZM_CAN || type == ZCAN || type == ZABORT) {
            result = XF_CANCEL;
            goto done;
        }
        if (type < 0 || type == ZNAK) {
            if (++errors > RETRY) {
                nc_log("CH%02d: ZMODEM 応答がない", t->no);
                goto done;
            }
            int r = infile ? send_pos(&z, ZRPOS, (uint32_t)off) : zshhdr(&z, ZRINIT, rinit);
            if (r < 0) goto disc;
            continue;
        }
        switch (type) {
        case ZRQINIT:
            if (zshhdr(&z, ZRINIT, rinit) < 0) goto disc;
            break;
        case ZSINIT: {
            size_t n;
            int r = zrecvdata(&z, blk, MAXSUB, &n);
            if (r == T_DISCONNECT) goto disc;
            if ((r < 0 ? send_pos(&z, ZNAK, 0) : send_pos(&z, ZACK, 1)) < 0) goto disc;
            break;
        }
        case ZFILE: {
            size_t n;
            int r = zrecvdata(&z, blk, MAXSUB - 1, &n);
            if (r == T_DISCONNECT) goto disc;
            if (r == ZM_CAN) {
                result = XF_CANCEL;
                goto done;
            }
            if (r < 0) {
                if (zshhdr(&z, ZRINIT, rinit) < 0) goto disc;
                break;
            }
            blk[n] = 0;
            if (infile) { /* 受け取り中に同じ ZFILE がもう一度来た */
                if (send_pos(&z, ZRPOS, (uint32_t)off) < 0) goto disc;
                break;
            }
            size_t nl = strlen((char *)blk);
            unsigned long sz = nl < n ? strtoul((char *)blk + nl + 1, NULL, 10) : 0;
            /* 2 つ目以降のファイル、名前がおかしいもの、大きすぎるものは飛ばしてもらう */
            if (got_file || nl == 0 || sz > (unsigned long)XFER_MAX_SIZE) {
                if (!got_file) {
                    if (nl == 0) badname = true;
                    else nc_log("CH%02d: ZMODEM %lu バイトは大きすぎる", t->no, sz);
                }
                if (send_pos(&z, ZSKIP, 0) < 0) goto disc;
                break;
            }
            const char *base = strrchr((char *)blk, '/');
            snprintf(name, namesz, "%s", base ? base + 1 : (char *)blk);
            want = sz;
            infile = true;
            off = 0;
            if (send_pos(&z, ZRPOS, 0) < 0) goto disc;
            break;
        }
        case ZDATA: {
            if (!infile) {
                if (zshhdr(&z, ZRINIT, rinit) < 0) goto disc;
                break;
            }
            if (hdr_pos(hdr) != off) {
                if (++errors > RETRY) goto done;
                term_purge(t, 200);
                z.ilen = z.ipos = 0;
                if (send_pos(&z, ZRPOS, (uint32_t)off) < 0) goto disc;
                break;
            }
            for (bool more = true; more;) {
                size_t n;
                int r = zrecvdata(&z, blk, MAXSUB, &n);
                if (r == T_DISCONNECT) goto disc;
                if (r == ZM_CAN) {
                    result = XF_CANCEL;
                    goto done;
                }
                if (r < 0) {
                    nc_log("CH%02d: ZMODEM %zu バイト目のデータが壊れている", t->no, off);
                    if (++errors > RETRY) goto done;
                    if (send_pos(&z, ZRPOS, (uint32_t)off) < 0) goto disc;
                    break;
                }
                errors = 0;
                if (off + n > (size_t)XFER_MAX_SIZE) goto done;
                if (off + n > cap) {
                    cap = cap ? cap * 2 : 65536;
                    while (cap < off + n) cap *= 2;
                    buf = realloc(buf, cap);
                }
                memcpy(buf + off, blk, n);
                off += n;
                if (r == ZCRCW || r == ZCRCQ)
                    if (send_pos(&z, ZACK, (uint32_t)off) < 0) goto disc;
                more = r == ZCRCG || r == ZCRCQ;
            }
            break;
        }
        case ZEOF:
            if (infile && hdr_pos(hdr) != off) break; /* 途中のデータが届いていない。ZRPOS を待たせる */
            if (infile) {
                infile = false;
                got_file = true;
            }
            if (zshhdr(&z, ZRINIT, rinit) < 0) goto disc;
            break;
        case ZFIN:
            send_pos(&z, ZFIN, 0);
            /* 送信側の "OO" を読み捨てる */
            for (int i = 0; i < 2 && zgetc(&z, 1000) >= 0; i++);
            result = got_file ? 0 : badname ? XF_BADNAME : XF_FAIL;
            goto done;
        default:
            break;
        }
    }
done:
    free(blk);
    if (result == 0) {
        if (want && want < off) off = want;
        *data = buf ? buf : malloc(1);
        *len = off;
    } else free(buf);
    return result;
disc:
    free(blk);
    free(buf);
    return T_DISCONNECT;
}

/* ------------------------------------------------------------ 送信 */

/* 受信側の ZRINIT を待つ。0 か XF_* か T_DISCONNECT。rxbuf に受信側のバッファの大きさ (0 は制限なし) */
static int wait_rinit(struct zm *z, bool first, unsigned *rxbuf) {
    static const unsigned char zero[4];
    unsigned char hdr[4];
    for (int tries = 0; tries < (first ? 12 : RETRY); tries++) {
        int type = zgethdr(z, 5000, hdr);
        if (type == T_DISCONNECT) return T_DISCONNECT;
        if (type == ZM_CAN || type == ZCAN || type == ZABORT) return XF_CANCEL;
        if (type == ZRINIT) {
            *rxbuf = hdr[0] | hdr[1] << 8;
            z->tx32 = hdr[ZF0] & CANFC32;
            z->escctl = hdr[ZF0] & ESCCTL;
            return 0;
        }
        if (type == ZCHALLENGE) {
            if (zshhdr(z, ZACK, hdr) < 0) return T_DISCONNECT;
            continue;
        }
        if (type == ZRQINIT) return XF_FAIL; /* 相手も送ろうとしている */
        if (first && zshhdr(z, ZRQINIT, zero) < 0) return T_DISCONNECT;
    }
    return XF_FAIL;
}

/* 1 ファイル送る。0 (飛ばされたときも) か XF_* か T_DISCONNECT */
static int send_file(struct zm *z, const struct xfile *f, int left, size_t left_bytes, unsigned rxbuf) {
    unsigned char hdr[4], info[512];
    int k = snprintf((char *)info, sizeof info, "%s", f->name) + 1;
    k += snprintf((char *)info + k, sizeof info - (size_t)k, "%zu %lo 100644 0 %d %zu", f->len, (unsigned long)f->mtime,
                  left, left_bytes);
    uint32_t pos = 0;
    int tries = 0;
    /* ZFILE を送って ZRPOS を待つ */
    for (;;) {
        if (++tries > RETRY) return XF_FAIL;
        unsigned char fh[4] = {0, 0, 0, ZCBIN};
        if (zsbhdr(z, ZFILE, fh) < 0 || zsdata(z, info, (size_t)k, ZCRCW) < 0) return T_DISCONNECT;
    again:;
        int type = zgethdr(z, 10000, hdr);
        if (type == T_DISCONNECT) return T_DISCONNECT;
        if (type == ZM_CAN || type == ZCAN || type == ZABORT || type == ZFERR) return XF_CANCEL;
        if (type == ZSKIP) return 0;
        if (type == ZRPOS) {
            pos = hdr_pos(hdr);
            break;
        }
        if (type == ZCRC) { /* 途中から続けるかを決めるための CRC */
            size_t n = hdr_pos(hdr);
            if (!n || n > f->len) n = f->len;
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 0; i < n; i++) c = crc32_up(c, f->data[i]);
            if (send_pos(z, ZCRC, ~c) < 0) return T_DISCONNECT;
            goto again;
        }
        /* ZRINIT / ZNAK / 時間切れ / 壊れたヘッダは ZFILE を送り直す */
    }
    /* データを流す。ZRPOS が来たらその位置から */
    int errors = 0;
    for (;;) {
        if (pos > f->len) pos = (uint32_t)f->len;
        pos_hdr(hdr, pos);
        if (zsbhdr(z, ZDATA, hdr) < 0) return T_DISCONNECT;
        bool sent = false;
        uint32_t since_ack = 0;
        int redo = 0; /* 1: ZRPOS で位置が変わった */
        while (pos < f->len || !sent) {
            size_t n = f->len - pos < SUBLEN ? f->len - pos : SUBLEN;
            int end = pos + n >= f->len ? ZCRCE : ZCRCG;
            if (rxbuf && since_ack + n >= rxbuf && end != ZCRCE) end = ZCRCW;
            if (zsdata(z, f->data + pos, n, end) < 0) return T_DISCONNECT;
            sent = true;
            pos += (uint32_t)n;
            since_ack += (uint32_t)n;
            /* 受信側から何か来ていれば読む (ZCRCW のあとは ZACK を待つ) */
            while (end == ZCRCW || zready(z)) {
                int type = zgethdr(z, end == ZCRCW ? 10000 : 100, hdr);
                if (type == T_DISCONNECT) return T_DISCONNECT;
                if (type == ZM_CAN || type == ZCAN || type == ZABORT || type == ZFERR) return XF_CANCEL;
                if (type == ZSKIP) return 0;
                if (type == ZRPOS) {
                    if (++errors > RETRY) return XF_FAIL;
                    pos = hdr_pos(hdr);
                    redo = 1;
                    break;
                }
                if (type == ZACK && end == ZCRCW) {
                    since_ack = 0;
                    break;
                }
                if (end == ZCRCW && type < 0) {
                    if (++errors > RETRY) return XF_FAIL;
                    pos -= (uint32_t)n;
                    redo = 1;
                    break;
                }
                if (end != ZCRCW && type == T_NODATA) break;
            }
            if (redo) break;
        }
        if (redo) continue;
        /* ZEOF を送って ZRINIT を待つ */
        for (int t2 = 0;; t2++) {
            if (t2 > RETRY) return XF_FAIL;
            pos_hdr(hdr, (uint32_t)f->len);
            if (zsbhdr(z, ZEOF, hdr) < 0) return T_DISCONNECT;
        wait:;
            int type = zgethdr(z, 10000, hdr);
            if (type == T_DISCONNECT) return T_DISCONNECT;
            if (type == ZM_CAN || type == ZCAN || type == ZABORT || type == ZFERR) return XF_CANCEL;
            if (type == ZRINIT || type == ZSKIP) return 0;
            if (type == ZACK) goto wait;
            if (type == ZRPOS) {
                if (++errors > RETRY) return XF_FAIL;
                pos = hdr_pos(hdr);
                redo = 1;
                break;
            }
        }
        if (!redo) return 0;
    }
}

int zm_send(struct term *t, const struct xfile *files, int n) {
    static const unsigned char zero[4];
    struct zm z = {.t = t};
    unsigned rxbuf = 0;
    /* "rz\r" は相手が UNIX のシェルのときに rz を起動させるためのもの */
    if (term_write_bin(t, "rz\r", 3) < 0 || zshhdr(&z, ZRQINIT, zero) < 0) return T_DISCONNECT;
    int r = wait_rinit(&z, true, &rxbuf);
    if (r) return r;
    size_t total = 0;
    for (int i = 0; i < n; i++) total += files[i].len;
    for (int i = 0; i < n; i++) {
        total -= files[i].len;
        if ((r = send_file(&z, &files[i], n - i - 1, total, rxbuf))) return r;
    }
    /* ZFIN を送り、受信側の ZFIN に "OO" で答える */
    for (int tries = 0; tries < 3; tries++) {
        if (zshhdr(&z, ZFIN, zero) < 0) return T_DISCONNECT;
        unsigned char hdr[4];
        int type = zgethdr(&z, 5000, hdr);
        if (type == T_DISCONNECT) return T_DISCONNECT;
        if (type == ZFIN) {
            term_write_bin(t, "OO", 2);
            break;
        }
    }
    return 0;
}
