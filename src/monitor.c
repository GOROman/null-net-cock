/*
 * 1 秒ごとの監視: 持ち時間を数える (LINESET の回線モード)、BUSY 時間、SDTIME のカウントダウン、スケジューラ
 */
#include "db.h"
#include "msg.h"
#include "nc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct pending {
    int no;
    enum notice_kind kind;
    char text[256];
};

/* 時間を数える。通知は g_lock を放してから送るので pend に貯める */
static int count_time(struct pending *pend, int max) {
    int n = 0;
    /* ノーマル回線が全部埋まったら、一番早くログインした (ノーマル回線の) 人から数え始める */
    bool all_busy = true, any_normal = false;
    int earliest = 0;
    for (int i = 1; i <= g_cfg.max_lines; i++) {
        if (g_sys.lines[i].mode != LM_NORMAL) continue;
        any_normal = true;
        struct online *o = &g_online[i];
        if (!o->used || o->uid < 0) {
            all_busy = false;
            continue;
        }
        if (!o->unlimited && (!earliest || o->since < g_online[earliest].since)) earliest = i;
    }
    for (int i = 1; i <= g_cfg.max_lines; i++) {
        struct online *o = &g_online[i];
        if (!o->used || o->uid < 0) continue;
        int mode = g_sys.lines[i].mode;
        o->counting = !o->unlimited && (g_sys.aoff || mode == LM_ALWAYS ||
                                        (mode == LM_NORMAL && any_normal && all_busy && i == earliest));
        if (!o->counting) continue;
        o->left--;
        if (o->left == 60 && !o->warned && n < max) {
            o->warned = true;
            pend[n].no = i;
            pend[n].kind = N_SYSTEM;
            snprintf(pend[n].text, sizeof pend[n].text, "%s%d%s", M(201), 60, M(202));
            n++;
        }
        if (o->left <= 0 && n < max) {
            o->left = 0;
            pend[n].no = i;
            pend[n].kind = N_TIMEUP;
            pend[n].text[0] = 0;
            n++;
        }
    }
    return n;
}

static void count_busy(time_t now) {
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_yday != g_sys.busy_day) {
        g_sys.busy_day = tm.tm_yday;
        g_sys.busy_sec = 0;
    }
    bool any = false, all = true;
    for (int i = 1; i <= g_cfg.max_lines; i++) {
        if (!g_sys.busy_line[i]) continue;
        any = true;
        if (!g_online[i].used) all = false;
    }
    if (any && all) g_sys.busy_sec++;
}

/* スポット型のスケジュールが今の分に当たるか */
static bool sched_hits(const struct sched *s, const struct tm *tm) {
    int h, m;
    if (!s->spot || sscanf(s->hhmm, "%d:%d", &h, &m) != 2 || h != tm->tm_hour || m != tm->tm_min) return false;
    if (s->kind == SK_DAILY) return true;
    if (s->kind == SK_WEEKDAY) return s->wday == tm->tm_wday;
    char today[16];
    strftime(today, sizeof today, "%Y-%m-%d", tm);
    return !strcmp(s->date, today);
}

static void *monitor_thread(void *arg) {
    int last_min = -1;
    for (;;) {
        sleep(1);
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        struct pending pend[MAX_LINES * 2 + 8];
        int np = 0;
        char broadcast[3][256];
        int nb = 0;

        pthread_mutex_lock(&g_lock);
        np = count_time(pend, (int)(sizeof pend / sizeof pend[0]));
        count_busy(now);
        /* スケジューラ (1 分に 1 回) */
        if (tm.tm_min != last_min) {
            last_min = tm.tm_min;
            int n;
            struct sched *list = sched_list(&n);
            for (int i = 0; i < n && nb < 3; i++) {
                if (!sched_hits(&list[i], &tm)) continue;
                snprintf(broadcast[nb++], sizeof broadcast[0], "*** %s ***", list[i].msg);
                if (list[i].down && !g_sys.down_at) {
                    g_sys.down_at = now + 300;
                    g_sys.aoff = true;
                }
            }
        }
        /* SDTIME: 30 秒ごとに残りを知らせ、0 で保存して終わる */
        bool down = false;
        if (g_sys.down_at) {
            long left = (long)(g_sys.down_at - now);
            if (left <= 0) down = true;
            else if (left % 30 == 0 && nb < 3) snprintf(broadcast[nb++], sizeof broadcast[0], "%s%ld%s", M(268), left, M(269));
        }
        if (down) {
            /* LCSET の「次のシステムダウンまで」を消す */
            for (int i = 0; i < g_nusers; i++)
                if (g_users[i].lcall == 1) g_users[i].lcall = 0;
            g_sys.aoff = false;
            db_save_all();
            nc_log("SDTIME: システムを止めます");
            exit(0);
        }
        pthread_mutex_unlock(&g_lock);

        for (int i = 0; i < np; i++) notice_push(pend[i].no, pend[i].kind, pend[i].text);
        for (int i = 0; i < nb; i++) notice_broadcast(N_SYSTEM, broadcast[i], 0);
    }
    return NULL;
}

void monitor_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, monitor_thread, NULL) == 0) pthread_detach(th);
}
