/*
 * WebSocket の回線 (ブラウザのソフトウェアモデム null-modem などから)
 *
 * 受け付けたら socketpair を作り、片方をふつうの回線 (telnet なし) としてセッションに渡す。
 * もう片方と WebSocket の間は、中継スレッドがフレームを付け外しする。
 * Cloudflare Tunnel などの中継から受ける想定なので、既定は 127.0.0.1 で待ち受ける。
 */
#include "nc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------ SHA-1 と Base64 (ハンドシェイク用) */

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1(const unsigned char *msg, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char *buf = calloc(1, total);
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)buf[off + 4 * i] << 24 | (uint32_t)buf[off + 4 * i + 1] << 16 |
                   (uint32_t)buf[off + 4 * i + 2] << 8 | buf[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    free(buf);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}

static void base64(const unsigned char *in, size_t n, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[k++] = tbl[v >> 18 & 63];
        out[k++] = tbl[v >> 12 & 63];
        out[k++] = i + 1 < n ? tbl[v >> 6 & 63] : '=';
        out[k++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    out[k] = 0;
}

/* ------------------------------------------------------------ ハンドシェイク */

static int write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ヘッダの値を探す (大文字小文字を区別しない) */
static bool header(const char *req, const char *name, char *out, size_t sz) {
    size_t nl = strlen(name);
    for (const char *p = req; (p = strchr(p, '\n')); ) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ') p++;
            size_t l = strcspn(p, "\r\n");
            snprintf(out, sz, "%.*s", (int)l, p);
            return true;
        }
    }
    return false;
}

/* HTTP の Upgrade を読んで 101 を返す。peer は中継が付けた元のアドレスがあればそれにする */
static int handshake(int fd, char *peer, size_t psz) {
    char req[8192];
    size_t len = 0;
    req[0] = 0; /* 最初の strstr の前に空にしておく (未初期化だとハンドシェイクを読まずに進むことがある) */
    while (!strstr(req, "\r\n\r\n")) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 10000) <= 0) return -1;
        ssize_t n = read(fd, req + len, sizeof req - 1 - len);
        if (n <= 0) return -1;
        len += (size_t)n;
        req[len] = 0;
        if (len >= sizeof req - 1) return -1;
    }
    char key[128], ip[64];
    if (!header(req, "Sec-WebSocket-Key", key, sizeof key)) {
        char up[32];
        if (header(req, "Upgrade", up, sizeof up)) /* 起動確認などのただの GET は記録しない */
            nc_log("WebSocket: Sec-WebSocket-Key の無い Upgrade を断りました (%.*s)", (int)strcspn(req, "\r\n"), req);
        static const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        write_all(fd, bad, sizeof bad - 1);
        return -1;
    }
    if (header(req, "CF-Connecting-IP", ip, sizeof ip) || header(req, "X-Forwarded-For", ip, sizeof ip))
        snprintf(peer, psz, "%.*s", (int)strcspn(ip, ","), ip);
    char buf[256];
    snprintf(buf, sizeof buf, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char dig[20];
    sha1((unsigned char *)buf, strlen(buf), dig);
    char acc[64];
    base64(dig, 20, acc);
    char resp[256];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     acc);
    return write_all(fd, resp, (size_t)n);
}

/* ------------------------------------------------------------ フレーム */

static int send_frame(int fd, int opcode, const unsigned char *data, size_t n) {
    unsigned char h[10];
    size_t hl = 2;
    h[0] = (unsigned char)(0x80 | opcode);
    if (n < 126) h[1] = (unsigned char)n;
    else if (n < 65536) {
        h[1] = 126;
        h[2] = (unsigned char)(n >> 8);
        h[3] = (unsigned char)n;
        hl = 4;
    } else {
        h[1] = 127;
        for (int i = 0; i < 8; i++) h[2 + i] = (unsigned char)((uint64_t)n >> (56 - 8 * i));
        hl = 10;
    }
    if (write_all(fd, h, hl) < 0) return -1;
    return n ? write_all(fd, data, n) : 0;
}

struct ws_state {
    unsigned char buf[65536 + 16];
    size_t len;
};

/* 受け取ったバイトからフレームを取り出す。データは out へ。-1: 閉じる */
static int take_frames(int ws, struct ws_state *st, int out) {
    for (;;) {
        if (st->len < 2) return 0;
        unsigned char *p = st->buf;
        int opcode = p[0] & 0x0F;
        bool masked = p[1] & 0x80;
        uint64_t n = p[1] & 0x7F;
        size_t hl = 2;
        if (n == 126) {
            if (st->len < 4) return 0;
            n = (uint64_t)p[2] << 8 | p[3];
            hl = 4;
        } else if (n == 127) {
            if (st->len < 10) return 0;
            n = 0;
            for (int i = 0; i < 8; i++) n = n << 8 | p[2 + i];
            hl = 10;
        }
        if (n > 65536) return -1;
        size_t need = hl + (masked ? 4 : 0) + (size_t)n;
        if (st->len < need) return 0;
        unsigned char *mask = p + hl, *data = p + hl + (masked ? 4 : 0);
        if (masked)
            for (size_t i = 0; i < n; i++) data[i] ^= mask[i % 4];
        if (opcode == 0x8) {
            send_frame(ws, 0x8, NULL, 0);
            return -1;
        }
        if (opcode == 0x9) send_frame(ws, 0xA, data, (size_t)n);
        else if (opcode <= 0x2 && n && write_all(out, data, (size_t)n) < 0) return -1;
        memmove(st->buf, st->buf + need, st->len - need);
        st->len -= need;
    }
}

/* WebSocket と socketpair の間を中継する */
static void relay(int ws, int pair) {
    struct ws_state *st = calloc(1, sizeof *st);
    for (;;) {
        struct pollfd pf[2] = {{ws, POLLIN, 0}, {pair, POLLIN, 0}};
        if (poll(pf, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(ws, st->buf + st->len, sizeof st->buf - st->len);
            if (n <= 0) break;
            st->len += (size_t)n;
            if (take_frames(ws, st, pair) < 0) break;
        }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            unsigned char buf[4096];
            ssize_t n = read(pair, buf, sizeof buf);
            if (n <= 0) {
                send_frame(ws, 0x8, NULL, 0);
                break;
            }
            if (send_frame(ws, 0x2, buf, (size_t)n) < 0) break;
        }
    }
    free(st);
}

/* ------------------------------------------------------------ 受け付け */

struct ws_conn {
    int fd;
    char peer[64];
};

static void *ws_session(void *arg) {
    struct ws_conn *wc = arg;
    char peer[64];
    snprintf(peer, sizeof peer, "%s", wc->peer);
    if (handshake(wc->fd, peer, sizeof peer) < 0) {
        close(wc->fd);
        free(wc);
        return NULL;
    }
    int no = online_alloc(peer);
    if (!no) {
        static const char busy[] = "\r\nただいま回線が混み合っています。\r\n";
        send_frame(wc->fd, 0x2, (const unsigned char *)busy, sizeof busy - 1);
        send_frame(wc->fd, 0x8, NULL, 0);
        close(wc->fd);
        free(wc);
        return NULL;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        online_free(no);
        close(wc->fd);
        free(wc);
        return NULL;
    }
    struct conn_arg *ca = calloc(1, sizeof *ca);
    ca->fd = sv[0];
    ca->no = no;
    ca->serial = true; /* telnet の処理はしない */
    snprintf(ca->peer, sizeof ca->peer, "%s (WS)", peer);
    snprintf(ca->speed, sizeof ca->speed, "WS");
    snprintf(ca->code, sizeof ca->code, "%s", g_cfg.ws_code[0] ? g_cfg.ws_code : "utf8");
    pthread_t th;
    if (pthread_create(&th, NULL, session_thread, ca) != 0) {
        close(sv[0]);
        close(sv[1]);
        online_free(no);
        free(ca);
        close(wc->fd);
        free(wc);
        return NULL;
    }
    pthread_detach(th);
    relay(wc->fd, sv[1]);
    close(sv[1]); /* セッション側は読み込みで終わりを知る */
    close(wc->fd);
    free(wc);
    return NULL;
}

static void *ws_accept(void *arg) {
    int ls = (int)(intptr_t)arg;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int fd = accept(ls, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            nc_log("WebSocket accept: %s", strerror(errno));
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        struct ws_conn *wc = calloc(1, sizeof *wc);
        wc->fd = fd;
        snprintf(wc->peer, sizeof wc->peer, "%s", inet_ntoa(peer.sin_addr));
        pthread_t th;
        if (pthread_create(&th, NULL, ws_session, wc) == 0) pthread_detach(th);
        else {
            close(fd);
            free(wc);
        }
    }
    return NULL;
}

int listen_tcp(const char *spec, int default_port);

void ws_start(void) {
    if (!g_cfg.ws_listen[0]) return;
    int ls = listen_tcp(g_cfg.ws_listen, 6869);
    if (ls < 0) {
        nc_log("WebSocket: %s で待ち受けできません: %s", g_cfg.ws_listen, strerror(errno));
        return;
    }
    nc_log("WebSocket %s で待ち受けを開始", g_cfg.ws_listen);
    pthread_t th;
    if (pthread_create(&th, NULL, ws_accept, (void *)(intptr_t)ls) == 0) pthread_detach(th);
}
