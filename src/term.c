/*
 * 端末の入出力: telnet の処理、文字コードの変換 (UTF-8 ⇔ SJIS)、1 行入力、ページ送り
 */
#include "msg.h"
#include "nc.h"

#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240 };
enum { TS_DATA, TS_IAC, TS_OPT, TS_SB, TS_SB_IAC };

void term_set_code(struct term *t, enum term_code code) {
    if (t->to_term != (iconv_t)-1) iconv_close(t->to_term);
    if (t->from_term != (iconv_t)-1) iconv_close(t->from_term);
    t->code = code;
    const char *name = code == CODE_SJIS ? "CP932" : "UTF-8";
    t->to_term = iconv_open(name, "UTF-8");
    t->from_term = iconv_open("UTF-8", name);
}

void term_init(struct term *t, int fd, int no, int notify_rd, const char *code, bool telnet) {
    memset(t, 0, sizeof *t);
    t->fd = fd;
    t->no = no;
    t->notify_rd = notify_rd;
    t->to_term = t->from_term = (iconv_t)-1;
    t->rows = 24;
    t->bs_one_col = true;
    t->last_input = time(NULL);
    t->telnet = telnet;
    term_set_code(t, strcmp(code, "utf8") == 0 ? CODE_UTF8 : CODE_SJIS);
    if (!telnet) return;
    /* サーバー側でエコーする (WILL ECHO)、GA は使わない (WILL SGA) */
    static const unsigned char greet[] = {IAC, WILL, 1, IAC, WILL, 3, IAC, DO, 3};
    term_write_raw(t, greet, sizeof greet);
}

void term_free(struct term *t) {
    if (t->to_term != (iconv_t)-1) iconv_close(t->to_term);
    if (t->from_term != (iconv_t)-1) iconv_close(t->from_term);
}

int term_write_raw(struct term *t, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len > 0 && !t->closed) {
        ssize_t n = write(t->fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            t->closed = true;
            return T_DISCONNECT;
        }
        p += n;
        len -= (size_t)n;
    }
    return t->closed ? T_DISCONNECT : T_OK;
}

/* UTF-8 を端末の文字コードにして送る (0xFF は IAC IAC に、\n は CRLF に) */
int term_print(struct term *t, const char *s) {
    /* 改行を CRLF に */
    size_t n = strlen(s);
    char *crlf = malloc(n * 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) crlf[k++] = '\r';
        crlf[k++] = s[i];
    }
    crlf[k] = 0;
    /* 文字コード変換 (変換できない文字は ? に) */
    size_t outsz = k * 2 + 16;
    char *out = malloc(outsz);
    char *ip = crlf, *op = out;
    size_t il = k, ol = outsz;
    iconv(t->to_term, NULL, NULL, NULL, NULL);
    while (il > 0) {
        if (iconv(t->to_term, &ip, &il, &op, &ol) == (size_t)-1) {
            if (errno == EILSEQ || errno == EINVAL) {
                /* 1 文字 (UTF-8 の 1 シーケンス) 飛ばして ? を出す */
                unsigned char c = (unsigned char)*ip;
                size_t skip = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
                if (skip > il) skip = il;
                ip += skip;
                il -= skip;
                if (ol > 0) { *op++ = '?'; ol--; }
            } else break;
        }
    }
    size_t outlen = (size_t)(op - out);
    /* IAC のエスケープ */
    unsigned char *esc = malloc(outlen * 2 + 1);
    size_t e = 0;
    for (size_t i = 0; i < outlen; i++) {
        esc[e++] = (unsigned char)out[i];
        if ((unsigned char)out[i] == IAC && t->telnet) esc[e++] = IAC;
    }
    int r = term_write_raw(t, esc, e);
    free(crlf);
    free(out);
    free(esc);
    return r;
}

int term_printf(struct term *t, const char *fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return term_print(t, buf);
}

/* telnet の制御を取り除いて、アプリ向けのバイトだけ返す */
/* シリアル: そのまま受け取り、キャリア断の「NO CARRIER」を探す (バイナリ転送中は探さない) */
static void serial_filter(struct term *t, const unsigned char *in, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char b = in[i];
        if ((t->carrier & CARRIER_TEXT) && !t->binary) {
            size_t l = strlen(t->tail);
            if (l >= sizeof t->tail - 1) {
                memmove(t->tail, t->tail + 1, l);
                l--;
            }
            t->tail[l] = b ? (char)b : ' ';
            t->tail[l + 1] = 0;
            if (strstr(t->tail, "NO CARRIER")) t->closed = true;
        }
        if (!t->binary) {
            if (t->after_cr && (b == 0 || b == '\n')) { t->after_cr = false; continue; }
            t->after_cr = b == '\r';
        }
        if (t->in_len < sizeof t->in) t->in[t->in_len++] = b;
    }
}

static void telnet_filter(struct term *t, const unsigned char *in, size_t n) {
    if (!t->telnet) {
        serial_filter(t, in, n);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char b = in[i];
        switch (t->telnet_state) {
        case TS_DATA:
            if (b == IAC) { t->telnet_state = TS_IAC; break; }
            if (!t->binary) {
                if (t->after_cr && (b == 0 || b == '\n')) { t->after_cr = false; break; }
                t->after_cr = b == '\r';
            }
            if (t->in_len < sizeof t->in) t->in[t->in_len++] = b;
            break;
        case TS_IAC:
            if (b == IAC) {
                if (t->in_len < sizeof t->in) t->in[t->in_len++] = b;
                t->telnet_state = TS_DATA;
            } else if (b >= WILL && b <= DONT) t->telnet_state = TS_OPT;
            else if (b == SB) t->telnet_state = TS_SB;
            else t->telnet_state = TS_DATA;
            break;
        case TS_OPT:
            /* 相手の要求には深入りせず黙る (ECHO と SGA はこちらから宣言済み) */
            t->telnet_state = TS_DATA;
            break;
        case TS_SB:
            if (b == IAC) t->telnet_state = TS_SB_IAC;
            break;
        case TS_SB_IAC:
            t->telnet_state = b == SE ? TS_DATA : TS_SB;
            break;
        }
    }
}

/* シリアルで DCD を見るとき、キャリアが落ちていたら true (調べられないポートは落ちていないことにする) */
static bool carrier_lost(struct term *t) {
    if (t->closed) return true;
    if (t->telnet || !(t->carrier & CARRIER_DCD)) return false;
    int st;
    if (ioctl(t->fd, TIOCMGET, &st) < 0) return false;
    if (!(st & TIOCM_CD)) t->closed = true;
    return t->closed;
}

/* 入力か通知が来るまで待つ。1: 入力あり 2: 通知あり 負: エラー */
static int wait_input(struct term *t) {
    for (;;) {
        time_t now = time(NULL);
        int idle = g_cfg.idle_timeout - (int)(now - t->last_input);
        if (g_cfg.idle_timeout > 0 && idle <= 0) return T_TIMEOUT;
        if (t->deadline && now >= t->deadline) return T_TIMEUP;
        if (t->deadline && !t->warned && t->deadline - now <= 60) {
            t->warned = t->warn_pending = true;
            return 2;
        }
        int wait_ms = 1000;
        struct pollfd pf[2] = {{t->fd, POLLIN, 0}, {t->notify_rd, POLLIN, 0}};
        int r = poll(pf, t->notify_rd >= 0 ? 2 : 1, wait_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            return T_DISCONNECT;
        }
        if (carrier_lost(t)) return T_DISCONNECT;
        if (r == 0) continue;
        if (pf[1].revents & POLLIN) {
            char c;
            (void)read(t->notify_rd, &c, 1);
            return 2;
        }
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            unsigned char buf[1024];
            ssize_t n = read(t->fd, buf, sizeof buf);
            if (n <= 0) {
                t->closed = true;
                return T_DISCONNECT;
            }
            t->last_input = time(NULL);
            telnet_filter(t, buf, (size_t)n);
            if (t->closed) return T_DISCONNECT;
            if (t->in_len > 0) return 1;
        }
    }
}

/* 表示幅 (全角は 2、半角カナは 1) */
static int char_width(const char *p, size_t len) {
    unsigned char c = (unsigned char)p[0];
    if (len == 1) return 1;
    /* U+FF61〜U+FF9F (半角カナ) は EF BD A1〜EF BE 9F */
    if (len == 3 && c == 0xEF && ((unsigned char)p[1] == 0xBD || (unsigned char)p[1] == 0xBE)) {
        unsigned cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F);
        if (cp >= 0xFF61 && cp <= 0xFF9F) return 1;
    }
    return 2;
}

/* 届いている通知を表示する。強制切断なら T_KICKED */
static int show_notices(struct term *t, const char *prompt, const char *typed, bool mask) {
    enum notice_kind kind;
    char text[512];
    bool shown = false;
    if (t->warn_pending) {
        t->warn_pending = false;
        term_printf(t, "\n%s%d%s", M(201), (int)(t->deadline - time(NULL)), M(202));
        shown = true;
    }
    while (notice_pop(t->no, &kind, text, sizeof text)) {
        if (kind == N_KICK) {
            term_printf(t, "\n\n*** %s ***\n", text);
            return T_KICKED;
        }
        if (kind == N_TIMEUP) return T_TIMEUP;
        term_printf(t, "\n%s", text);
        shown = true;
    }
    if (shown && prompt) {
        term_printf(t, "\n%s", prompt);
        if (mask) {
            /* UTF-8 の 1 文字ごとに、全角なら＊、半角なら * */
            for (const char *p = typed; *p;) {
                size_t l = 1;
                while (p[l] && ((unsigned char)p[l] & 0xC0) == 0x80) l++;
                term_print(t, char_width(p, l) == 2 ? "＊" : "*");
                p += l;
            }
        } else term_print(t, typed);
    }
    return T_OK;
}

/* 端末の 1 文字ぶんのバイト数 (SJIS / UTF-8) */
static size_t term_char_len(struct term *t, const unsigned char *p, size_t avail) {
    unsigned char c = p[0];
    if (t->code == CODE_SJIS) {
        bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
        return lead ? (avail >= 2 ? 2 : 0) : 1;
    }
    size_t need = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return avail >= need ? need : 0;
}

int term_readline(struct term *t, const char *prompt, char *out, size_t outsz, int flags) {
    bool mask = flags & RL_MASK;
    char line[LINE_MAX_BYTES] = "";
    size_t len = 0;
    if (prompt && term_print(t, prompt) < 0) return T_DISCONNECT;
    for (;;) {
        /* たまっている入力を 1 文字ずつ処理 */
        while (t->in_len > 0) {
            unsigned char c = t->in[0];
            size_t cl = 1;
            if (c == '\r' || c == '\n') {
                memmove(t->in, t->in + 1, --t->in_len);
                term_print(t, "\n");
                line[len] = 0;
                if (flags & RL_UPPER) str_upper(line);
                if (!(flags & RL_RAW)) str_trim(line);
                snprintf(out, outsz, "%s", line);
                return T_OK;
            }
            if (c == 0x08 || c == 0x7F) {
                if (len > 0) {
                    /* UTF-8 の 1 文字ぶん戻す */
                    size_t s = len - 1;
                    while (s > 0 && ((unsigned char)line[s] & 0xC0) == 0x80) s--;
                    int w = char_width(line + s, len - s);
                    if (w == 2 && !t->bs_one_col) w = 1; /* BS 1 個で全角 1 文字戻る端末 */
                    len = s;
                    line[len] = 0;
                    if (!(flags & RL_NOECHO))
                        for (int i = 0; i < w; i++) term_write_raw(t, "\b \b", 3);
                }
            } else if (c == 0x15) { /* Ctrl-U: 行を消す */
                while (len > 0) {
                    size_t s = len - 1;
                    while (s > 0 && ((unsigned char)line[s] & 0xC0) == 0x80) s--;
                    int w = char_width(line + s, len - s);
                    if (w == 2 && !t->bs_one_col) w = 1;
                    len = s;
                    if (!(flags & RL_NOECHO))
                        for (int i = 0; i < w; i++) term_write_raw(t, "\b \b", 3);
                }
                line[0] = 0;
            } else if (c >= 0x20) {
                cl = term_char_len(t, t->in, t->in_len);
                if (cl == 0) break; /* 2 バイト文字の途中: 続きを待つ */
                /* UTF-8 に変換して行に足す */
                char u8[8];
                char *ip = (char *)t->in, *op = u8;
                size_t il = cl, ol = sizeof u8;
                iconv(t->from_term, NULL, NULL, NULL, NULL);
                if (iconv(t->from_term, &ip, &il, &op, &ol) != (size_t)-1) {
                    size_t ul = (size_t)(op - u8);
                    if (len + ul < sizeof line - 1 && len + ul < outsz - 1) {
                        memcpy(line + len, u8, ul);
                        len += ul;
                        line[len] = 0;
                        if (flags & RL_NOECHO) {
                        } else if (mask) term_print(t, char_width(u8, ul) == 2 ? "＊" : "*"); /* 全角は全角の＊ */
                        else term_write_raw(t, t->in, cl); /* 受け取ったまま返す */
                    } else term_write_raw(t, "\a", 1);
                }
            }
            memmove(t->in, t->in + cl, t->in_len - cl);
            t->in_len -= cl;
        }
        int r = wait_input(t);
        if (r == 2) {
            int k = show_notices(t, prompt, line, mask);
            if (k < 0) return k;
        } else if (r < 0) {
            return r;
        }
    }
}

/* ページ送り: 行数を数え、1 ページたまったら止める。0: 続ける 1: 中止 負: エラー */
int term_more(struct term *t, int *line_count) {
    if (++*line_count < t->rows - 1) return 0;
    *line_count = 0;
    char a[16];
    int r = term_readline(t, "-- 続きます (Enter:次 Q:中止) --", a, sizeof a, RL_UPPER);
    if (r < 0) return r;
    return a[0] == 'Q' ? 1 : 0;
}

int term_yesno(struct term *t, const char *prompt) {
    char a[16];
    char p[256];
    snprintf(p, sizeof p, "%s (Y/N)? ", prompt);
    int r = term_readline(t, p, a, sizeof a, RL_UPPER);
    if (r < 0) return r;
    return a[0] == 'Y' ? 1 : 0;
}

/* ------------------------------------------------------------ バイナリ転送 */

int term_getc(struct term *t, int timeout_ms) {
    for (;;) {
        if (t->in_len > 0) {
            int c = t->in[0];
            memmove(t->in, t->in + 1, --t->in_len);
            return c;
        }
        struct pollfd pf = {t->fd, POLLIN, 0};
        int r = poll(&pf, 1, timeout_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            return T_DISCONNECT;
        }
        if (r == 0) return T_NODATA;
        unsigned char buf[2048];
        ssize_t n = read(t->fd, buf, sizeof buf);
        if (n <= 0) {
            t->closed = true;
            return T_DISCONNECT;
        }
        t->last_input = time(NULL);
        telnet_filter(t, buf, (size_t)n);
        /* telnet の制御だけだったときは、同じ待ち時間でもう一度待つ */
    }
}

int term_write_bin(struct term *t, const void *buf, size_t len) {
    const unsigned char *p = buf;
    unsigned char out[4096];
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        out[k++] = p[i];
        if (p[i] == IAC && t->telnet) out[k++] = IAC;
        if (k >= sizeof out - 2) {
            if (term_write_raw(t, out, k) < 0) return T_DISCONNECT;
            k = 0;
        }
    }
    return k ? term_write_raw(t, out, k) : T_OK;
}

/* telnet の BINARY オプション (0) を両方向で使う / やめる */
void term_set_binary(struct term *t, bool on) {
    unsigned char seq[] = {IAC, on ? WILL : WONT, 0, IAC, on ? DO : DONT, 0};
    if (t->telnet) term_write_raw(t, seq, sizeof seq);
    t->binary = on;
    t->after_cr = false;
}

/* 相手が送るのをやめるまで (quiet_ms の間なにも来なくなるまで) 受信を捨てる */
void term_purge(struct term *t, int quiet_ms) {
    t->in_len = 0;
    while (term_getc(t, quiet_ms) >= 0) t->in_len = 0;
}
