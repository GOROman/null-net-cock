/*
 * ホストまわりの SYSOP コマンド: REPORT / KEYLOCK / ALARM / DEBUG / HFCONT
 *
 * HFCONT (ファイル管理) は、データディレクトリの下の files/ だけを扱う。そこから外へは出られない。
 */
#include "session.h"
#include "xfer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* ------------------------------------------------------------ REPORT */

static const char *signup_name(int m) {
    return m == SIGNUP_AUTO ? "自動発行" : m == SIGNUP_MANUAL ? "手作業で発行" : "オンラインでは受け付けない";
}

int cmd_report(struct sess *s, const char *arg) {
    pthread_mutex_lock(&g_lock);
    int napps, pending = 0, users = 0, msgs = 0;
    struct application *a = app_list(&napps);
    for (int i = 0; i < napps; i++)
        if (!a[i].issued_id) pending++;
    for (int i = 0; i < g_nusers; i++)
        if (user_get(i)) users++;
    for (int i = 0; i < g_nmsgs; i++)
        if (!g_msgs[i].deleted) msgs++;
    long body = total_body_bytes(), pds = total_pds_bytes();
    struct system t = g_sys;
    pthread_mutex_unlock(&g_lock);

    char started[32], now[32];
    fmt_time(t.started, started, sizeof started);
    fmt_time(time(NULL), now, sizeof now);
    struct statvfs vfs;
    unsigned long long free_kb = 0;
    if (statvfs(g_cfg.data_dir, &vfs) == 0) free_kb = (unsigned long long)vfs.f_bavail * vfs.f_frsize / 1024;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    long mem_kb = ru.ru_maxrss / 1024; /* macOS はバイト */
#else
    long mem_kb = ru.ru_maxrss;        /* Linux は KB */
#endif
    char call[16];
    if (t.chat_call_level >= 255) snprintf(call, sizeof call, "OFF");
    else snprintf(call, sizeof call, "%d", t.chat_call_level);
    CHK(out(s, "%s%d\n%s%d/%d\n%s%d\n%s%ld\n%s%ld (ゲスト %ld)\n", M(271), pending, M(272), users, MAX_USERS, M(273),
            msgs, M(274), (long)(time(NULL) - t.started) / 60, M(275), t.total_logins, t.guest_logins));
    CHK(out(s, "%s%ld / %ld\n%s%ld / %ld\n%s%s): %llu KB\n%s%ld KB\n", M(276), body, t.board_size, M(277), pds,
            t.pds_size, M(278), g_cfg.data_dir, free_kb, M(279), mem_kb));
    CHK(out(s, "%s%s\n%s%s\n%s%s%s\n%s%s / %s / %s / %s\n%s%s\n%s%s\n%s\n", M(281), now, M(389), started, M(379),
            signup_name(t.signup), "", M(380), t.imode_menu ? "メニュー" : "コマンド",
            t.imode_menu_always ? "毎回メニュー" : "-", t.imode_chat_noecho ? "エコーなし" : "エコー",
            t.imode_chat_byid ? "ID" : "名前", M(381), t.imode_chat_esc ? "ESC" : "-", M(378), call, M(283)));
    return out(s, "%s%s\n", M(386), M(t.keylock ? 388 : 387));
}

/* ------------------------------------------------------------ KEYLOCK / ALARM */

/* ホストのキーボードロックを外す (ロックはホストコンソールでかける) */
int cmd_keylock(struct sess *s, const char *arg) {
    pthread_mutex_lock(&g_lock);
    bool locked = g_sys.keylock;
    pthread_mutex_unlock(&g_lock);
    CHK(out(s, "%s%s\n", M(386), M(locked ? 388 : 387)));
    if (!locked) return 0;
    int r = yn(s, 383);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    g_sys.keylock = false;
    db_save_sys();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 384);
}

/* 本体のタイマー予約を消す (この実装には本体のタイマーが無いので、表示だけ) */
int cmd_alarm(struct sess *s, const char *arg) { return outm_nl(s, 294); }

/* ------------------------------------------------------------ DEBUG */

int cmd_debug(struct sess *s, const char *arg) {
    for (;;) {
        char a[64];
        CHK(ask(s, 337, a, sizeof a, 0));
        if (!a[0] || !strcasecmp(a, "q")) return 0;
        char buf[4096] = "";
        size_t l = 0;
        pthread_mutex_lock(&g_lock);
        if (!strcasecmp(a, "lines")) {
            for (int i = 1; i <= g_cfg.max_lines; i++) {
                struct online *o = &g_online[i];
                if (o->used)
                    l += (size_t)snprintf(buf + l, sizeof buf - l, "%s%d uid=%d left=%d cnt=%d chat=%d place=%s peer=%s\n",
                                          M(338), i, o->uid, o->left, o->counting, o->chat, o->place, o->peer);
            }
        } else if (!strcasecmp(a, "sys")) {
            l += (size_t)snprintf(buf + l, sizeof buf - l,
                                  "%snet_id=%s users=%d msgs=%d aoff=%d down_at=%ld busy=%ld manager=%d\n", M(338),
                                  g_sys.net_id, g_nusers, g_nmsgs, g_sys.aoff, (long)g_sys.down_at, g_sys.busy_sec,
                                  g_sys.manager);
        } else if (!strcasecmp(a, "boards")) {
            for (int i = 0; i < MAX_BOARDS; i++)
                if (g_boards[i].used)
                    l += (size_t)snprintf(buf + l, sizeof buf - l, "%s%d %s type=%c next=%d n=%d\n", M(338), i,
                                          g_boards[i].index, g_boards[i].type, g_boards[i].next_seq, board_count(i));
        } else snprintf(buf, sizeof buf, "%slines / sys / boards / q\n", M(338));
        pthread_mutex_unlock(&g_lock);
        CHK(out(s, "%s", buf));
    }
}

/* ------------------------------------------------------------ HFCONT */

struct hf {
    char root[600];
    char cwd[600];          /* root からの相対 ("" が root) */
    char marks[16][256];    /* 印を付けたファイル (cwd からの相対ではなく root からの相対) */
    int nmarks;
};

/* 1 つの名前として安全か (区切りや .. を含まない) */
static bool safe_name(const char *n) {
    return n[0] && !strchr(n, '/') && strcmp(n, ".") && strcmp(n, "..") && n[0] != '.';
}

static void hf_path(const struct hf *h, const char *name, char *out, size_t sz) {
    if (h->cwd[0]) snprintf(out, sz, "%s/%s/%s", h->root, h->cwd, name);
    else snprintf(out, sz, "%s/%s", h->root, name);
}

static int hf_list(struct sess *s, struct hf *h) {
    char dir[900];
    hf_path(h, ".", dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return out(s, "==== ディレクトリを開けません ====\n");
    CHK(out(s, "/%s\n", h->cwd));
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1200];
        hf_path(h, e->d_name, p, sizeof p);
        struct stat st;
        if (stat(p, &st) < 0) continue;
        char rel[512];
        snprintf(rel, sizeof rel, "%s%s%s", h->cwd, h->cwd[0] ? "/" : "", e->d_name);
        bool marked = false;
        for (int i = 0; i < h->nmarks; i++)
            if (!strcmp(h->marks[i], rel)) marked = true;
        char ts[32];
        fmt_time(st.st_mtime, ts, sizeof ts);
        int r = S_ISDIR(st.st_mode) ? out(s, "  %-24s <DIR>      %s\n", e->d_name, ts)
                                    : out(s, "%c %-24s %10lld %s\n", marked ? '*' : ' ', e->d_name,
                                          (long long)st.st_size, ts);
        if (r < 0 || (r = paging(s, &count, false))) {
            closedir(d);
            return r < 0 ? r : 0;
        }
    }
    closedir(d);
    return 0;
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return -1;
    FILE *b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return -1;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    return fclose(b);
}

static int hf_help(struct sess *s) {
    return out(s, "L:一覧 C 名前:ディレクトリへ (C .. で上へ) K 名前:ディレクトリを作る D 名前:削除 R 旧 新:名前を変える\n"
                  "P 名前 先:複写 (先はディレクトリ名か新しい名前) S 名前:印を付ける E 名前:印を外す M:印の一覧\n"
                  "Y / X:受け取る (YMODEM / XMODEM) G:印を付けたものを YMODEM で送る Q:終わる\n");
}

int cmd_hfcont(struct sess *s, const char *arg) {
    struct hf *h = calloc(1, sizeof *h);
    snprintf(h->root, sizeof h->root, "%s/files", g_cfg.data_dir);
    mkdir(h->root, 0755);
    int r = hf_list(s, h);
    while (r >= 0) {
        char line[256], a1[128] = "", a2[128] = "";
        char prompt[700];
        snprintf(prompt, sizeof prompt, "HFCONT /%s (?:説明)>", h->cwd);
        if ((r = ask_str(s, prompt, line, sizeof line, 0)) < 0) break;
        char c = (char)toupper((unsigned char)line[0]);
        sscanf(line + (line[0] ? 1 : 0), " %127s %127s", a1, a2);
        char p1[1200], p2[1200];
        if (c == 'Q' || !line[0]) break;
        switch (c) {
        case 'L':
            r = hf_list(s, h);
            break;
        case 'C':
            if (!strcmp(a1, "..")) {
                char *slash = strrchr(h->cwd, '/');
                if (slash) *slash = 0;
                else h->cwd[0] = 0;
            } else if (safe_name(a1)) {
                hf_path(h, a1, p1, sizeof p1);
                struct stat st;
                if (stat(p1, &st) == 0 && S_ISDIR(st.st_mode) && strlen(h->cwd) + strlen(a1) + 2 < sizeof h->cwd) {
                    size_t l = strlen(h->cwd);
                    snprintf(h->cwd + l, sizeof h->cwd - l, "%s%s", l ? "/" : "", a1);
                } else r = out(s, "==== そのディレクトリはありません ====\n");
            } else r = out(s, "==== そのディレクトリはありません ====\n");
            if (r >= 0) r = hf_list(s, h);
            break;
        case 'K':
            if (!safe_name(a1)) break;
            hf_path(h, a1, p1, sizeof p1);
            r = mkdir(p1, 0755) == 0 ? outm_nl(s, 220) : out(s, "==== 作れません: %s ====\n", strerror(errno));
            break;
        case 'D': {
            if (!safe_name(a1)) break;
            hf_path(h, a1, p1, sizeof p1);
            CHK(out(s, "%s ", a1));
            int yes = yn(s, 238);
            if (yes <= 0) {
                r = yes;
                break;
            }
            r = (remove(p1) == 0) ? outm_nl(s, 204) : out(s, "==== 削除できません: %s ====\n", strerror(errno));
            break;
        }
        case 'R':
            if (!safe_name(a1) || !safe_name(a2)) break;
            hf_path(h, a1, p1, sizeof p1);
            hf_path(h, a2, p2, sizeof p2);
            r = rename(p1, p2) == 0 ? outm_nl(s, 220) : out(s, "==== 変えられません: %s ====\n", strerror(errno));
            break;
        case 'P': {
            if (!safe_name(a1) || !safe_name(a2)) break;
            hf_path(h, a1, p1, sizeof p1);
            hf_path(h, a2, p2, sizeof p2);
            struct stat st;
            if (stat(p2, &st) == 0 && S_ISDIR(st.st_mode)) {
                size_t l = strlen(p2);
                snprintf(p2 + l, sizeof p2 - l, "/%s", a1);
            }
            r = copy_file(p1, p2) == 0 ? outm_nl(s, 220) : out(s, "==== 複写できません ====\n");
            break;
        }
        case 'S':
        case 'E': {
            if (!safe_name(a1)) break;
            char rel[512];
            snprintf(rel, sizeof rel, "%s%s%s", h->cwd, h->cwd[0] ? "/" : "", a1);
            int found = -1;
            for (int i = 0; i < h->nmarks; i++)
                if (!strcmp(h->marks[i], rel)) found = i;
            if (c == 'S' && found < 0 && h->nmarks < 16) snprintf(h->marks[h->nmarks++], sizeof h->marks[0], "%s", rel);
            if (c == 'E' && found >= 0) {
                memmove(h->marks[found], h->marks[found + 1], sizeof h->marks[0] * (size_t)(h->nmarks - found - 1));
                h->nmarks--;
            }
            break;
        }
        case 'M':
            for (int i = 0; i < h->nmarks && r >= 0; i++) r = out(s, " %s\n", h->marks[i]);
            break;
        case 'Y':
        case 'X': {
            char name[64] = "";
            if (c == 'X') {
                if ((r = ask(s, 311, name, sizeof name, 0)) < 0) break;
                if (!safe_name(name)) break;
            }
            if ((r = outm_nl(s, c == 'Y' ? 304 : 303)) < 0) break;
            unsigned char *data = NULL;
            size_t len = 0;
            int x = xfer_recv(s->t, c == 'Y' ? XR_YMODEM : XR_CRC, name, sizeof name, &data, &len);
            if (x < 0) {
                r = x;
                break;
            }
            if (x || !safe_name(name)) {
                free(data);
                r = outm_nl(s, x == XF_CANCEL ? 309 : 308);
                break;
            }
            hf_path(h, name, p1, sizeof p1);
            FILE *f = fopen(p1, "wb");
            if (f) {
                fwrite(data, 1, len, f);
                fclose(f);
            }
            free(data);
            r = f ? out(s, "%s  %s %zu bytes\n", M(319), name, len) : out(s, "==== 書き込めません ====\n");
            break;
        }
        case 'G': {
            if (!h->nmarks) {
                r = outm_nl(s, 326);
                break;
            }
            struct xfile f[16];
            unsigned char *bufs[16];
            int n = 0;
            for (int i = 0; i < h->nmarks; i++) {
                snprintf(p1, sizeof p1, "%s/%s", h->root, h->marks[i]);
                FILE *fp = fopen(p1, "rb");
                if (!fp) continue;
                fseek(fp, 0, SEEK_END);
                long sz = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (sz < 0 || sz > XFER_MAX_SIZE) {
                    fclose(fp);
                    continue;
                }
                bufs[n] = malloc((size_t)sz + 1);
                size_t got = fread(bufs[n], 1, (size_t)sz, fp);
                fclose(fp);
                const char *base = strrchr(h->marks[i], '/');
                f[n] = (struct xfile){base ? base + 1 : h->marks[i], bufs[n], got, time(NULL)};
                n++;
            }
            if ((r = outm_nl(s, 328)) < 0) break;
            int x = n ? xfer_send(s->t, XS_YMODEM, f, n) : XF_FAIL;
            for (int i = 0; i < n; i++) free(bufs[i]);
            if (x < 0) {
                r = x;
                break;
            }
            if (!x) h->nmarks = 0;
            r = outm_nl(s, x == 0 ? 332 : 331);
            break;
        }
        default:
            r = hf_help(s);
        }
    }
    free(h);
    return r < 0 ? r : 0;
}
