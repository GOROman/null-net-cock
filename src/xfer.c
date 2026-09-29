/*
 * XMODEM / YMODEM
 *
 * - 128 バイトのブロックは SOH、1024 バイトは STX で始まる。ブロック番号、その 1 の補数、データ、チェック
 *   (SUM はデータの合計の下位 8 ビット、CRC は CRC-16/XMODEM を上位バイトから) が続く
 * - 受信側が NAK (SUM) / 'C' (CRC) / 'G' (YMODEM-g) を送って始める。再送を決めるのは受信側
 * - YMODEM の 0 番ブロックはファイル名・サイズ・更新時刻。全部 NUL の 0 番ブロックがバッチの終わり
 * - 待っている間に ZMODEM のヘッダ (ZPAD ZDLE) が来たら zmodem.c に任せる
 */
#include "xfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SOH = 0x01, STX = 0x02, EOT = 0x04, ACK = 0x06, NAK = 0x15, CAN = 0x18, SUB = 0x1A, ZPAD = '*' };
#define RETRY 10

static unsigned short crc16(const unsigned char *p, size_t n) {
    unsigned short crc = 0;
    while (n--) {
        crc ^= (unsigned short)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = crc & 0x8000 ? (unsigned short)((crc << 1) ^ 0x1021) : (unsigned short)(crc << 1);
    }
    return crc;
}

static int put1(struct term *t, unsigned char c) { return term_write_bin(t, &c, 1); }

static void cancel(struct term *t) {
    static const unsigned char c[] = {CAN, CAN, CAN, CAN, CAN, 8, 8, 8, 8, 8};
    term_write_bin(t, c, sizeof c);
}

/* ------------------------------------------------------------ 受信 */

/* 1 ブロックの残り (番号から後) を読む。1: 正常 0: 壊れている 負: 切断 */
static int read_block(struct term *t, int size, bool crc, int *blk, unsigned char *data) {
    int b1 = term_getc(t, 1000), b2 = term_getc(t, 1000);
    if (b1 < 0 || b2 < 0) return b1 == T_NODATA || b2 == T_NODATA ? 0 : T_DISCONNECT;
    for (int i = 0; i < size; i++) {
        int c = term_getc(t, 1000);
        if (c == T_NODATA) return 0;
        if (c < 0) return c;
        data[i] = (unsigned char)c;
    }
    bool ok;
    if (crc) {
        int h = term_getc(t, 1000), l = term_getc(t, 1000);
        if (h < 0 || l < 0) return h == T_DISCONNECT || l == T_DISCONNECT ? T_DISCONNECT : 0;
        ok = crc16(data, (size_t)size) == ((h << 8) | l);
    } else {
        int s = term_getc(t, 1000);
        if (s < 0) return s == T_DISCONNECT ? T_DISCONNECT : 0;
        unsigned char sum = 0;
        for (int i = 0; i < size; i++) sum = (unsigned char)(sum + data[i]);
        ok = sum == s;
    }
    if ((b1 ^ b2) != 0xFF) ok = false;
    *blk = b1;
    return ok ? 1 : 0;
}

int xfer_recv(struct term *t, int proto, char *name, size_t namesz, unsigned char **data, size_t *len) {
    bool crc = proto != XR_SUM, ymodem = proto >= XR_YMODEM, gmode = proto == XR_YMODEM_G;
    unsigned char start = gmode ? 'G' : crc ? 'C' : NAK;
    unsigned char block[1024];
    size_t cap = 0, n = 0, want = 0;
    unsigned char *buf = NULL;
    int expect = ymodem ? 0 : 1; /* 次に来るブロック番号 */
    bool header = ymodem;        /* YMODEM の 0 番ブロックを待っている */
    bool got_file = false, started = false, eot_once = false;
    int errors = 0, result = XF_FAIL;

    term_set_binary(t, true);
    if (put1(t, start) < 0) goto disc;
    for (;;) {
        int c = term_getc(t, started ? 10000 : 3000);
        if (c == T_DISCONNECT) goto disc;
        if (c == T_NODATA) {
            if (++errors > (started ? RETRY : 20)) {
                nc_log("CH%02d: XMODEM 応答がない", t->no);
                goto fail;
            }
            if (put1(t, started ? (gmode ? start : NAK) : start) < 0) goto disc;
            continue;
        }
        if (c == CAN) {
            if (term_getc(t, 1000) == CAN) {
                result = XF_CANCEL;
                goto done;
            }
            continue;
        }
        if (c == EOT) {
            if (getenv("NNC_XDEBUG")) nc_log("EOT (once=%d n=%zu)", eot_once, n);
            started = true;
            if (ymodem && !eot_once) {
                eot_once = true;
                if (put1(t, NAK) < 0) goto disc;
                continue;
            }
            if (put1(t, ACK) < 0) goto disc;
            got_file = true;
            if (!ymodem) {
                result = 0;
                goto done;
            }
            /* 次のヘッダ (バッチの終わり) を求める */
            header = true;
            expect = 0;
            eot_once = false;
            errors = 0;
            if (put1(t, start) < 0) goto disc;
            continue;
        }
        if (c == ZPAD && !started) {
            /* sz (ZMODEM) の ZRQINIT。ZMODEM で受け取る */
            do c = term_getc(t, 1000);
            while (c == ZPAD);
            if (c == T_DISCONNECT) goto disc;
            if (c != CAN) continue;
            free(buf);
            int r = zm_recv(t, name, namesz, data, len);
            if (r == T_DISCONNECT) return r;
            if (r > 0 && r != XF_CANCEL) cancel(t);
            term_purge(t, 500);
            term_set_binary(t, false);
            return r;
        }
        if (c != SOH && c != STX) continue; /* ゴミは読み捨てる */
        started = true;
        int size = c == SOH ? 128 : 1024, blk = -1;
        int r = read_block(t, size, crc, &blk, block);
        if (r < 0) goto disc;
        if (r == 0) {
            nc_log("CH%02d: XMODEM ブロック %d が壊れている", t->no, blk);
            if (gmode || ++errors > RETRY) goto fail;
            term_purge(t, 300);
            if (put1(t, NAK) < 0) goto disc;
            continue;
        }
        errors = 0;
        if (getenv("NNC_XDEBUG")) nc_log("blk %d size %d header=%d", blk, size, header);
        if (header && blk == 0) {
            if (!block[0]) { /* バッチの終わり */
                if (!got_file) nc_log("CH%02d: YMODEM ファイルが無いままバッチが終わった", t->no);
                put1(t, ACK);
                result = got_file ? 0 : XF_FAIL;
                goto done;
            }
            if (got_file) { /* 2 つ目以降のファイルは受け取らない */
                cancel(t);
                result = 0;
                goto done;
            }
            size_t nl = strnlen((char *)block, (size_t)size);
            if (nl == 0 || nl >= (size_t)size - 1) {
                cancel(t);
                result = XF_BADNAME;
                goto done;
            }
            /* パスを取り除く */
            const char *base = strrchr((char *)block, '/');
            snprintf(name, namesz, "%s", base ? base + 1 : (char *)block);
            want = (size_t)strtoul((char *)block + nl + 1, NULL, 10);
            header = false;
            expect = 1;
            if (!gmode && put1(t, ACK) < 0) goto disc;
            if (put1(t, start) < 0) goto disc;
            continue;
        }
        if (blk == ((expect - 1) & 0xFF)) { /* 同じブロックが再送された */
            if (!gmode && put1(t, ACK) < 0) goto disc;
            continue;
        }
        if (blk != (expect & 0xFF)) {
            nc_log("CH%02d: XMODEM ブロック番号が違う (%d / 期待 %d)", t->no, blk, expect & 0xFF);
            cancel(t);
            goto fail;
        }
        if (n + (size_t)size > (size_t)XFER_MAX_SIZE) {
            cancel(t);
            goto fail;
        }
        if (n + (size_t)size > cap) {
            cap = (cap ? cap * 2 : 65536);
            while (cap < n + (size_t)size) cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + n, block, (size_t)size);
        n += (size_t)size;
        expect++;
        if (!gmode && put1(t, ACK) < 0) goto disc;
    }
fail:
    result = result == XF_CANCEL ? XF_CANCEL : XF_FAIL;
done:
    term_purge(t, 500);
    term_set_binary(t, false);
    if (result == 0) {
        if (want && want <= n) n = want;
        else if (!want)
            while (n > 0 && buf[n - 1] == SUB) n--;
        *data = buf ? buf : malloc(1);
        *len = n;
    } else free(buf);
    return result;
disc:
    free(buf);
    return T_DISCONNECT;
}

/* ------------------------------------------------------------ 送信 */

/* 受信側の開始の合図を待つ。'C' / NAK / 'G'、ZMODEM の rz は 'Z'、中止は XF_CANCEL */
static int wait_start(struct term *t, int timeout_s) {
    for (int i = 0; i < timeout_s; i++) {
        int c = term_getc(t, 1000);
        if (c == T_DISCONNECT) return T_DISCONNECT;
        if (c == 'C' || c == NAK || c == 'G') return c;
        if (c == ZPAD) {
            do c = term_getc(t, 1000);
            while (c == ZPAD);
            if (c == T_DISCONNECT) return T_DISCONNECT;
            if (c == CAN) return 'Z';
        }
        if (c == CAN && term_getc(t, 1000) == CAN) return -XF_CANCEL;
    }
    return -XF_FAIL;
}

/* ブロックを送って ACK を待つ (g モードは待たない) */
static int send_block(struct term *t, int blk, const unsigned char *data, int size, bool crc, bool gmode) {
    unsigned char pkt[3 + 1024 + 2];
    int k = 0;
    pkt[k++] = size == 1024 ? STX : SOH;
    pkt[k++] = (unsigned char)blk;
    pkt[k++] = (unsigned char)~blk;
    memcpy(pkt + k, data, (size_t)size);
    k += size;
    if (crc) {
        unsigned short c = crc16(data, (size_t)size);
        pkt[k++] = (unsigned char)(c >> 8);
        pkt[k++] = (unsigned char)c;
    } else {
        unsigned char sum = 0;
        for (int i = 0; i < size; i++) sum = (unsigned char)(sum + data[i]);
        pkt[k++] = sum;
    }
    for (int tries = 0; tries < RETRY; tries++) {
        if (term_write_bin(t, pkt, (size_t)k) < 0) return T_DISCONNECT;
        if (gmode) return 0;
        for (;;) {
            int c = term_getc(t, 10000);
            if (c == T_DISCONNECT) return T_DISCONNECT;
            if (c == ACK) return 0;
            if (c == CAN) {
                if (term_getc(t, 1000) == CAN) return XF_CANCEL;
                continue;
            }
            if (c == NAK || c == T_NODATA) break; /* 再送 */
        }
    }
    return XF_FAIL;
}

static int send_eot(struct term *t, bool gmode) {
    for (int tries = 0; tries < RETRY; tries++) {
        if (put1(t, EOT) < 0) return T_DISCONNECT;
        int c = term_getc(t, 10000);
        if (c == T_DISCONNECT) return T_DISCONNECT;
        if (c == ACK) return 0;
        if (c == CAN && term_getc(t, 1000) == CAN) return XF_CANCEL;
        /* NAK (YMODEM の 1 回目) やタイムアウトはもう一度 EOT */
    }
    return gmode ? 0 : XF_FAIL;
}

static int send_data(struct term *t, const struct xfile *f, int bsize, bool crc, bool gmode) {
    unsigned char buf[1024];
    int blk = 1;
    for (size_t off = 0; off < f->len; off += (size_t)bsize, blk++) {
        size_t n = f->len - off < (size_t)bsize ? f->len - off : (size_t)bsize;
        int size = bsize;
        /* 最後の端数が 128 以下なら 128 バイトのブロックで送る (YMODEM) */
        if (bsize == 1024 && n <= 128 && crc) size = 128;
        memcpy(buf, f->data + off, n);
        memset(buf + n, SUB, (size_t)size - n);
        int r = send_block(t, blk, buf, size, crc, gmode);
        if (r) return r;
    }
    return send_eot(t, gmode);
}

int xfer_send(struct term *t, int proto, const struct xfile *files, int n) {
    int r;
    term_set_binary(t, true);
    term_purge(t, 200);
    int c = proto == XS_ZMODEM ? 'Z' : wait_start(t, 60);
    if (c < 0) {
        r = c == T_DISCONNECT ? T_DISCONNECT : -c;
        goto out;
    }
    if (c == 'Z') {
        r = zm_send(t, files, proto == XS_YMODEM || proto == XS_ZMODEM ? n : 1);
        goto out;
    }
    bool crc = c != NAK, gmode = c == 'G';
    if (proto != XS_YMODEM) {
        int bsize = proto == XS_X1K && crc ? 1024 : 128;
        r = send_data(t, &files[0], bsize, crc, false);
        goto out;
    }
    for (int i = 0; i <= n; i++) {
        /* 0 番ブロック: ファイル名 NUL サイズ 空白 更新時刻 (8 進)。i == n はバッチの終わり */
        unsigned char hdr[128] = {0};
        if (i < n) {
            int k = snprintf((char *)hdr, sizeof hdr, "%s", files[i].name) + 1;
            snprintf((char *)hdr + k, sizeof hdr - (size_t)k, "%zu %lo", files[i].len, (unsigned long)files[i].mtime);
        }
        if (i > 0) {
            c = wait_start(t, 60);
            if (c < 0) {
                r = c == T_DISCONNECT ? T_DISCONNECT : -c;
                goto out;
            }
        }
        r = send_block(t, 0, hdr, 128, true, gmode);
        if (i == n || r) {
            if (gmode && r == XF_FAIL && i == n) r = 0; /* 最後の ACK が無い受信側もある */
            goto out;
        }
        c = wait_start(t, 60);
        if (c < 0) {
            r = c == T_DISCONNECT ? T_DISCONNECT : -c;
            goto out;
        }
        if ((r = send_data(t, &files[i], 1024, true, gmode))) goto out;
    }
    r = 0;
out:
    if (r > 0 && r != XF_CANCEL) cancel(t);
    if (r != T_DISCONNECT) {
        term_purge(t, 500);
        term_set_binary(t, false);
    }
    return r;
}
