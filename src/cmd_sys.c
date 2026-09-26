/*
 * SYSOP: 回線と時間・スケジュール (LINESET / CTIME / LBUSY / ACCESS / SDTIME / SCSET / SCLIST)
 */
#include "session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const WDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

/* 「〜しませんね (y/n)」形式。N のときだけ 1 */
static int yn_neg(struct sess *s, int id) {
    char a[16];
    CHK(ask(s, id, a, sizeof a, RL_UPPER));
    return a[0] == 'N';
}

/* ------------------------------------------------------------ LINESET */

static const char *mode_name(int m) {
    return m == LM_NOTIMEOUT ? "タイムアウトなし" : m == LM_ALWAYS ? "常時タイムカウント" : "ノーマル";
}

int cmd_lineset(struct sess *s, const char *arg) {
    for (int i = 1; i <= g_cfg.max_lines; i++) {
        pthread_mutex_lock(&g_lock);
        int lv = g_sys.lines[i].level, mode = g_sys.lines[i].mode;
        pthread_mutex_unlock(&g_lock);
        CHK(out(s, "回線 %2d  レベル %3d  %s\n", i, lv, mode_name(mode)));
        int v;
        CHK(ask_int(s, 153, lv, &v));
        char a[8];
        CHK(ask_str(s, "時間のモード (N:ノーマル T:タイムアウトなし A:常時タイムカウント Q:終わる)＞", a, sizeof a,
                    RL_UPPER));
        pthread_mutex_lock(&g_lock);
        if (v >= 0 && v <= 255) g_sys.lines[i].level = v;
        if (a[0] == 'N') g_sys.lines[i].mode = LM_NORMAL;
        if (a[0] == 'T') g_sys.lines[i].mode = LM_NOTIMEOUT;
        if (a[0] == 'A') g_sys.lines[i].mode = LM_ALWAYS;
        db_save_sys();
        pthread_mutex_unlock(&g_lock);
        if (a[0] == 'Q') break;
    }
    return outm_nl(s, 220);
}

/* ------------------------------------------------------------ CTIME */

static int show_ctime(struct sess *s) {
    char buf[4096];
    size_t l = 0;
    pthread_mutex_lock(&g_lock);
    l += (size_t)snprintf(buf + l, sizeof buf - l, "回線  ");
    for (int i = 1; i <= g_cfg.max_lines && i <= 32; i++)
        l += (size_t)snprintf(buf + l, sizeof buf - l, "%d%c ", i, g_sys.chat_limited[i] ? '*' : '-');
    l += (size_t)snprintf(buf + l, sizeof buf - l, "\n     0         1         2\n     012345678901234567890123\n");
    for (int d = 0; d < 7; d++) {
        l += (size_t)snprintf(buf + l, sizeof buf - l, "%s  ", WDAY[d]);
        for (int h = 0; h < 24; h++) buf[l++] = g_sys.ctime_tab[d][h] ? 'X' : 'O';
        buf[l++] = '\n';
    }
    buf[l] = 0;
    pthread_mutex_unlock(&g_lock);
    return out(s, "%s", buf);
}

/* チャット (と時間制限付きのボード) を使える時間帯。* の回線は X の時間帯にチャットできない */
int cmd_ctime(struct sess *s, const char *arg) {
    for (;;) {
        CHK(show_ctime(s));
        char a[64];
        CHK(ask_str(s,
                    "L 回線:対象を切り替え / 曜日(0-6) 時-時 X:禁止 S:許可 / W 曜日:前の曜日を写す / Enter:終わる＞",
                    a, sizeof a, RL_UPPER));
        if (!a[0]) break;
        int d, h1, h2, line;
        char op;
        pthread_mutex_lock(&g_lock);
        if (sscanf(a, "L %d", &line) == 1 && line >= 1 && line <= MAX_LINES)
            g_sys.chat_limited[line] = !g_sys.chat_limited[line];
        else if (sscanf(a, "W %d", &d) == 1 && d >= 1 && d <= 6)
            memcpy(g_sys.ctime_tab[d], g_sys.ctime_tab[d - 1], 24);
        else if (sscanf(a, "%d %d-%d %c", &d, &h1, &h2, &op) == 4 && d >= 0 && d <= 6 && h1 >= 0 && h2 <= 23 &&
                 h1 <= h2 && (op == 'X' || op == 'S'))
            for (int h = h1; h <= h2; h++) g_sys.ctime_tab[d][h] = op == 'X';
        db_save_sys();
        pthread_mutex_unlock(&g_lock);
    }
    return outm_nl(s, 220);
}

/* ------------------------------------------------------------ LBUSY */

int cmd_lbusy(struct sess *s, const char *arg) {
    for (;;) {
        char buf[512];
        size_t l = 0;
        pthread_mutex_lock(&g_lock);
        for (int i = 1; i <= g_cfg.max_lines && i <= 32; i++)
            l += (size_t)snprintf(buf + l, sizeof buf - l, "%d%c ", i, g_sys.busy_line[i] ? '*' : '-');
        pthread_mutex_unlock(&g_lock);
        CHK(out(s, "%s\n", buf));
        char a[16];
        CHK(ask_str(s, "切り替える回線の番号 (Enter:終わる)＞", a, sizeof a, 0));
        if (!a[0]) break;
        int n = atoi(a);
        pthread_mutex_lock(&g_lock);
        if (n >= 1 && n <= MAX_LINES) g_sys.busy_line[n] = !g_sys.busy_line[n];
        db_save_sys();
        pthread_mutex_unlock(&g_lock);
    }
    return outm_nl(s, 220);
}

/* ------------------------------------------------------------ ACCESS */

/* 回線ごとの本日 / 昨日のアクセス時間と回数、昨日の使用率、平均使用率 (記録のある日の平均) */
int cmd_access(struct sess *s, const char *arg) {
    enum { MAXLOG = 512 };
    struct logent *e = malloc(sizeof *e * MAXLOG);
    pthread_mutex_lock(&g_lock);
    int n = log_all(e, MAXLOG);
    long busy = g_sys.busy_sec;
    pthread_mutex_unlock(&g_lock);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    time_t today = mktime(&tm), yesterday = today - 86400;
    time_t oldest = n ? e[0].in : now;
    CHK(out(s, "回線  本日(分) 回数  昨日(分) 回数  昨日の使用率  平均使用率\n"));
    for (int line = 1; line <= g_cfg.max_lines; line++) {
        long t_today = 0, t_yest = 0, t_all = 0;
        int c_today = 0, c_yest = 0;
        for (int i = 0; i < n; i++) {
            if (e[i].line != line) continue;
            long d = (long)(e[i].out - e[i].in);
            t_all += d;
            if (e[i].in >= today) t_today += d, c_today++;
            else if (e[i].in >= yesterday) t_yest += d, c_yest++;
        }
        double days = (double)(now - oldest) / 86400.0;
        if (days < 1) days = 1;
        CHK(out(s, " %2d   %6ld %5d   %6ld %5d      %5.1f%%      %5.1f%%\n", line, t_today / 60, c_today, t_yest / 60,
                c_yest, t_yest * 100.0 / 86400, t_all * 100.0 / (86400 * days)));
    }
    free(e);
    return out(s, "本日の BUSY 時間 %ld 分\n", busy / 60);
}

/* ------------------------------------------------------------ SDTIME */

int cmd_sdtime(struct sess *s, const char *arg) {
    int m;
    CHK(ask_int(s, 265, -1, &m));
    if (m != 0 && m != 1 && m != 3 && m != 5) return 0;
    pthread_mutex_lock(&g_lock);
    g_sys.down_at = m ? time(NULL) + m * 60 : 0;
    g_sys.aoff = m != 0;
    pthread_mutex_unlock(&g_lock);
    nc_log("CH%02d: SDTIME %d 分", s->no, m);
    return outm_nl(s, m ? 266 : 267);
}

/* ------------------------------------------------------------ SCSET / SCLIST */

static int ask_line(struct sess *s, int id, char *buf, size_t sz) { return ask(s, id, buf, sz, 0); }

int cmd_scset(struct sess *s, const char *arg) {
    int n;
    pthread_mutex_lock(&g_lock);
    sched_list(&n);
    pthread_mutex_unlock(&g_lock);
    if (n >= 32) return outm_nl(s, 251);
    struct sched sc = {0};
    int r = yn(s, 252);
    if (r < 0) return r;
    sc.spot = r;
    if (!sc.spot) {
        CHK(ask_line(s, 253, sc.start, sizeof sc.start));
        CHK(ask_line(s, 254, sc.end, sizeof sc.end));
    } else {
        if ((r = yn(s, 259)) < 0) return r;
        if (r) {
            sc.kind = SK_DATE;
            CHK(ask_line(s, 257, sc.date, sizeof sc.date));
        } else {
            if ((r = yn(s, 260)) < 0) return r;
            if (r) sc.kind = SK_DAILY;
            else {
                sc.kind = SK_WEEKDAY;
                CHK(ask_int(s, 261, 0, &sc.wday));
            }
        }
        CHK(ask_line(s, 262, sc.hhmm, sizeof sc.hhmm));
        if ((r = yn_neg(s, 263)) < 0) return r;
        sc.down = r;
        if (sc.down) {
            CHK(ask_int(s, 264, 0, &sc.timer_wday));
            CHK(ask_line(s, 262, sc.timer_hhmm, sizeof sc.timer_hhmm));
        }
    }
    CHK(ask_line(s, 255, sc.msg, sizeof sc.msg));
    if (!sc.msg[0]) return 0;
    pthread_mutex_lock(&g_lock);
    int ok = sched_add(&sc);
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, ok < 0 ? 251 : 256);
}

static void sched_text(const struct sched *c, char *buf, size_t sz) {
    if (!c->spot) snprintf(buf, sz, "%2d. %s〜%s  %s", c->no, c->start, c->end, c->msg);
    else if (c->kind == SK_DATE) snprintf(buf, sz, "%2d. %s %s%s  %s", c->no, c->date, c->hhmm, c->down ? "*" : "", c->msg);
    else if (c->kind == SK_DAILY) snprintf(buf, sz, "%2d. 毎日 %s%s  %s", c->no, c->hhmm, c->down ? "*" : "", c->msg);
    else snprintf(buf, sz, "%2d. %s %s%s  %s", c->no, WDAY[c->wday % 7], c->hhmm, c->down ? "*" : "", c->msg);
}

int cmd_sclist(struct sess *s, const char *arg) {
    for (int i = 0;;) {
        int n;
        char text[320];
        pthread_mutex_lock(&g_lock);
        struct sched *list = sched_list(&n);
        if (i < n) sched_text(&list[i], text, sizeof text);
        pthread_mutex_unlock(&g_lock);
        if (i >= n) break;
        CHK(out(s, "%s\n", text));
        int r = yn_neg(s, 258);
        if (r < 0) return r;
        if (r) {
            pthread_mutex_lock(&g_lock);
            sched_del(i);
            pthread_mutex_unlock(&g_lock);
            CHK(outm_nl(s, 204));
        } else i++;
    }
    return 0;
}

/* オープニングで出す、期間型のスケジュールのメッセージ */
int sched_opening(struct sess *s) {
    char today[16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(today, sizeof today, "%Y-%m-%d", &tm);
    char buf[2048] = "";
    pthread_mutex_lock(&g_lock);
    int n;
    struct sched *list = sched_list(&n);
    for (int i = 0; i < n; i++)
        if (!list[i].spot && strcmp(today, list[i].start) >= 0 && strcmp(today, list[i].end) <= 0) {
            size_t l = strlen(buf);
            snprintf(buf + l, sizeof buf - l, "%s\n", list[i].msg);
        }
    pthread_mutex_unlock(&g_lock);
    return buf[0] ? out(s, "%s", buf) : 0;
}
