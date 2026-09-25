/*
 * モデム回線: 初期化 → 着信待ち → 応答 (ATA) → セッション → 切断 (DTR / +++ATH0) → 再初期化 を繰り返す
 *
 * 1 回線 1 スレッド。null-bbs で実機 (IODATA DFML-560 / aiwa PV-PF24MK2) に合わせた間の取り方を使う:
 * OK の直後は 200ms (ATZ / AT&F の後は 1 秒) 待ってから次を送る、RING の直後は 300ms 待って ATA、
 * ATA の後にまた RING が来たら ATA を送り直す。
 */
#include "nc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

struct modem {
    const struct modem_cfg *cfg;
    int fd;
    char buf[1024];
    size_t len;
};

static void msleep(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static speed_t baud_const(int baud) {
    switch (baud) {
    case 300: return B300;
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return B9600;
    }
}

static void set_dtr(int fd, bool on) {
    int bit = TIOCM_DTR;
    ioctl(fd, on ? TIOCMBIS : TIOCMBIC, &bit);
}

static int open_port(const struct modem_cfg *c) {
    int fd = open(c->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, baud_const(c->baud));
        cfsetospeed(&tio, baud_const(c->baud));
        /* DCD は TIOCMGET で自分で見るので、制御線は無視させる (DCD が無くても読み書きできる) */
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~(tcflag_t)CRTSCTS;
        tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF);
        if (!strcmp(c->flow, "hardware")) tio.c_cflag |= CRTSCTS;
        else if (!strcmp(c->flow, "software")) tio.c_iflag |= IXON | IXOFF;
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio); /* pty などは失敗してもよい */
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    set_dtr(fd, true);
    return fd;
}

static int send_str(struct modem *m, const char *s) {
    char shown[160];
    snprintf(shown, sizeof shown, "%s", s);
    str_trim(shown);
    if (shown[0]) nc_log("CH%02d: → %s", m->cfg->line, shown);
    size_t n = strlen(s);
    const char *p = s;
    while (n > 0) {
        ssize_t w = write(m->fd, p, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* 受け取っているものを捨てる */
static void drain(struct modem *m, int quiet_ms) {
    char tmp[512];
    struct pollfd pf = {m->fd, POLLIN, 0};
    while (poll(&pf, 1, quiet_ms) > 0 && read(m->fd, tmp, sizeof tmp) > 0) {}
    m->len = 0;
}

/* 1 行読む。1: 読めた 0: 時間切れ -1: ポートのエラー */
static int read_line(struct modem *m, char *out, size_t sz, int timeout_ms) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        for (size_t i = 0; i < m->len; i++) {
            if (m->buf[i] != '\r' && m->buf[i] != '\n') continue;
            size_t l = i < sz - 1 ? i : sz - 1;
            memcpy(out, m->buf, l);
            out[l] = 0;
            memmove(m->buf, m->buf + i + 1, m->len - i - 1);
            m->len -= i + 1;
            str_trim(out);
            if (!out[0]) {
                i = (size_t)-1;
                continue;
            }
            nc_log("CH%02d: ← %s", m->cfg->line, out);
            return 1;
        }
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int left = timeout_ms - (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) return 0;
        struct pollfd pf = {m->fd, POLLIN, 0};
        int r = poll(&pf, 1, left);
        if (r < 0 && errno != EINTR) return -1;
        if (r <= 0) continue;
        if (pf.revents & (POLLERR | POLLNVAL)) return -1;
        if (m->len >= sizeof m->buf - 1) m->len = 0;
        ssize_t n = read(m->fd, m->buf + m->len, sizeof m->buf - 1 - m->len);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return -1;
        m->len += (size_t)n;
    }
}

enum result { MR_OTHER, MR_OK, MR_ERROR, MR_RING, MR_CONNECT, MR_NOCARRIER, MR_BUSY, MR_NODIALTONE, MR_NOANSWER };

static enum result parse_result(const char *line, char *speed, size_t sz) {
    char u[128];
    snprintf(u, sizeof u, "%s", line);
    str_upper(u);
    if (!strcmp(u, "OK") || !strcmp(u, "0")) return MR_OK;
    if (!strcmp(u, "ERROR") || !strcmp(u, "4")) return MR_ERROR;
    if (!strcmp(u, "RING") || !strcmp(u, "2")) return MR_RING;
    if (!strcmp(u, "NO CARRIER") || !strcmp(u, "3")) return MR_NOCARRIER;
    if (!strcmp(u, "BUSY") || !strcmp(u, "7")) return MR_BUSY;
    if (!strncmp(u, "NO DIAL", 7) || !strcmp(u, "6")) return MR_NODIALTONE;
    if (!strcmp(u, "NO ANSWER") || !strcmp(u, "8")) return MR_NOANSWER;
    if (!strcmp(u, "1") || !strncmp(u, "CONNECT", 7)) {
        const char *p = line + (u[0] == '1' ? 1 : 7);
        while (*p == ' ') p++;
        snprintf(speed, sz, "%s", *p ? p : "CONNECT");
        return MR_CONNECT;
    }
    return MR_OTHER;
}

/* 初期化コマンドを 1 つずつ送って OK を待つ。0: 成功 */
static int init_modem(struct modem *m) {
    if (send_str(m, "\r") < 0) return -1;
    msleep(300);
    drain(m, 100);
    for (int i = 0; i < m->cfg->ninit; i++) {
        const char *cmd = m->cfg->init[i];
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            char line[256];
            snprintf(line, sizeof line, "%s\r", cmd);
            if (send_str(m, line) < 0) return -1;
            for (;;) {
                int r = read_line(m, line, sizeof line, 3000);
                if (r < 0) return -1;
                if (r == 0) break;
                char sp[8];
                enum result res = parse_result(line, sp, sizeof sp);
                if (res == MR_OK) {
                    ok = true;
                    break;
                }
                if (res == MR_ERROR) {
                    nc_log("CH%02d: %s が ERROR になりました", m->cfg->line, cmd);
                    return 1;
                }
            }
        }
        if (!ok) {
            nc_log("CH%02d: モデムが応答しません (%s)", m->cfg->line, cmd);
            return 1;
        }
        char u[16];
        snprintf(u, sizeof u, "%s", cmd);
        str_upper(u);
        msleep(!strncmp(u, "ATZ", 3) || !strncmp(u, "AT&F", 4) ? 1000 : 200);
    }
    return 0;
}

/* 着信を待って応答する。1: CONNECT (speed に速度) 0: 接続できず再初期化 -1: ポートのエラー */
static int wait_call(struct modem *m, char *speed, size_t sz) {
    const struct modem_cfg *c = m->cfg;
    int rings = 0;
    time_t last_ring = 0, answered = 0;
    for (;;) {
        char line[256];
        int r = read_line(m, line, sizeof line, 1000);
        if (r < 0) return -1;
        time_t now = time(NULL);
        if (answered && now - answered > c->connect_timeout) {
            nc_log("CH%02d: 接続できませんでした (タイムアウト)", c->line);
            send_str(m, "\r"); /* 応答中の処理を中止させる */
            return 0;
        }
        if (!answered && last_ring && now - last_ring > 10) { /* 呼び出しが止んだ */
            rings = 0;
            last_ring = 0;
            online_set_place(c->line, "");
        }
        if (r == 0) continue;
        switch (parse_result(line, speed, sz)) {
        case MR_RING: {
            rings++;
            last_ring = now;
            char place[32];
            snprintf(place, sizeof place, "RING x%d", rings);
            online_set_place(c->line, place);
            bool retry = !c->answer_auto && answered;
            if (!c->answer_auto && (retry || rings >= (c->rings > 0 ? c->rings : 1))) {
                if (retry) nc_log("CH%02d: ATA が受け付けられなかったので送り直します", c->line);
                msleep(300); /* RING の直後に送るとモデムが取りこぼす */
                if (send_str(m, "ATA\r") < 0) return -1;
                answered = now;
            } else if (c->answer_auto && !answered) answered = now;
            break;
        }
        case MR_CONNECT:
            return 1;
        case MR_NOCARRIER:
        case MR_BUSY:
        case MR_NOANSWER:
        case MR_NODIALTONE:
        case MR_ERROR:
            if (answered) {
                nc_log("CH%02d: 接続できませんでした (%s)", c->line, line);
                return 0;
            }
            break;
        default:
            break;
        }
    }
}

static void hangup(struct modem *m) {
    if (!strcmp(m->cfg->hangup, "escape")) {
        nc_log("CH%02d: → +++ / ATH0 (回線を切ります)", m->cfg->line);
        msleep(1200);
        (void)!write(m->fd, "+++", 3);
        msleep(1200);
        (void)!write(m->fd, "ATH0\r", 5);
        msleep(1500);
    } else {
        /* &D2 のモデムは DTR を落とすと回線を切る */
        nc_log("CH%02d: DTR OFF (回線を切ります)", m->cfg->line);
        set_dtr(m->fd, false);
        msleep(600);
        set_dtr(m->fd, true);
        msleep(1500);
    }
    drain(m, 300);
}

static void *modem_thread(void *arg) {
    const struct modem_cfg *c = arg;
    int no = c->line;
    for (;;) {
        struct modem m = {.cfg = c, .fd = open_port(c)};
        if (m.fd < 0) {
            nc_log("CH%02d: %s を開けません: %s", no, c->path, strerror(errno));
            sleep(10);
            continue;
        }
        nc_log("CH%02d: %s を開きました (%d bps)", no, c->path, c->baud);
        for (;;) {
            int r = init_modem(&m);
            if (r < 0) break;
            if (r > 0) {
                sleep(30);
                continue;
            }
            char speed[32] = "";
            r = wait_call(&m, speed, sizeof speed);
            if (r < 0) break;
            if (r == 0) continue;
            nc_log("CH%02d: 着信 CONNECT %s", no, speed);
            online_set_place(no, "");
            msleep(c->connect_delay_ms);
            drain(&m, 50); /* 接続直後の雑音を捨てる */
            char peer[300];
            snprintf(peer, sizeof peer, "%s %s", c->path, speed);
            if (!online_alloc_at(no, peer)) {
                nc_log("CH%02d: 回線が使用中です", no);
                hangup(&m);
                continue;
            }
            struct conn_arg ca = {.fd = m.fd, .no = no, .serial = true};
            snprintf(ca.peer, sizeof ca.peer, "%s", peer);
            snprintf(ca.speed, sizeof ca.speed, "%s", speed);
            snprintf(ca.code, sizeof ca.code, "%s", c->code);
            if (!strcmp(c->carrier, "dcd") || !strcmp(c->carrier, "both")) ca.carrier |= CARRIER_DCD;
            if (!strcmp(c->carrier, "text") || !strcmp(c->carrier, "both")) ca.carrier |= CARRIER_TEXT;
            session_run(&ca);
            online_free(no);
            hangup(&m);
        }
        nc_log("CH%02d: ポートのエラー。開き直します", no);
        close(m.fd);
        sleep(5);
    }
    return NULL;
}

void modem_start(void) {
    for (int i = 0; i < g_cfg.nmodems; i++) {
        struct modem_cfg *c = &g_cfg.modems[i];
        if (!c->path[0]) continue;
        pthread_t th;
        if (pthread_create(&th, NULL, modem_thread, c) == 0) pthread_detach(th);
    }
}
