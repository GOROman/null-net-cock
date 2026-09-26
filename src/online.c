/*
 * 在室者 (回線ごとの利用状況) と、回線どうしの通知 (電報・チャット・全体放送・強制切断)
 */
#include "nc.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct online g_online[MAX_LINES + 1];

static bool is_modem_line(int no) {
    for (int i = 0; i < g_cfg.nmodems; i++)
        if (g_cfg.modems[i].line == no) return true;
    return false;
}

static int claim(int no, const char *peer);

/* 空いている回線を確保する。満杯なら 0 */
int online_alloc(const char *peer) {
    pthread_mutex_lock(&g_lock);
    int no = 0;
    for (int i = 1; i <= g_cfg.max_lines && i <= MAX_LINES; i++) {
        if (!g_online[i].used && !is_modem_line(i)) {
            no = i;
            break;
        }
    }
    no = claim(no, peer);
    pthread_mutex_unlock(&g_lock);
    return no;
}

int online_alloc_at(int no, const char *peer) {
    pthread_mutex_lock(&g_lock);
    no = no >= 1 && no <= MAX_LINES && !g_online[no].used ? claim(no, peer) : 0;
    pthread_mutex_unlock(&g_lock);
    return no;
}

/* g_lock を持って呼ぶ */
static int claim(int no, const char *peer) {
    if (no) {
        struct online *o = &g_online[no];
        memset(o, 0, sizeof *o);
        o->used = true;
        o->no = no;
        o->since = time(NULL);
        o->uid = -1;
        snprintf(o->peer, sizeof o->peer, "%s", peer);
        int p[2];
        if (pipe(p) == 0) {
            fcntl(p[0], F_SETFL, O_NONBLOCK);
            fcntl(p[1], F_SETFL, O_NONBLOCK);
            o->notify_rd = p[0];
            o->notify_wr = p[1];
        } else {
            o->notify_rd = o->notify_wr = -1;
        }
    }
    return no;
}

void online_free(int no) {
    pthread_mutex_lock(&g_lock);
    struct online *o = &g_online[no];
    if (o->notify_rd >= 0) close(o->notify_rd);
    if (o->notify_wr >= 0) close(o->notify_wr);
    memset(o, 0, sizeof *o);
    pthread_mutex_unlock(&g_lock);
}

void online_set_user(int no, const char *id, const char *handle) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_online[no].id, sizeof g_online[no].id, "%s", id);
    snprintf(g_online[no].handle, sizeof g_online[no].handle, "%s", handle);
    pthread_mutex_unlock(&g_lock);
}

void online_set_place(int no, const char *place) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_online[no].place, sizeof g_online[no].place, "%s", place);
    pthread_mutex_unlock(&g_lock);
}

/* 通知を積む (相手の入力待ちを起こす)。あふれたら捨てて false */
bool notice_push(int no, enum notice_kind kind, const char *text) {
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    struct online *o = &g_online[no];
    if (o->used && o->q_len < (int)(sizeof o->q / sizeof o->q[0])) {
        int i = (o->q_head + o->q_len) % (int)(sizeof o->q / sizeof o->q[0]);
        o->q[i].kind = kind;
        snprintf(o->q[i].text, sizeof o->q[i].text, "%s", text);
        o->q_len++;
        if (o->notify_wr >= 0) (void)!write(o->notify_wr, "!", 1);
        ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool notice_pop(int no, enum notice_kind *kind, char *text, size_t sz) {
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    struct online *o = &g_online[no];
    if (o->used && o->q_len > 0) {
        *kind = o->q[o->q_head].kind;
        snprintf(text, sz, "%s", o->q[o->q_head].text);
        o->q_head = (o->q_head + 1) % (int)(sizeof o->q / sizeof o->q[0]);
        o->q_len--;
        ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* ログイン中の全員 (except を除く) に送る。届いた人数を返す */
int notice_broadcast(enum notice_kind kind, const char *text, int except) {
    int n = 0;
    for (int i = 1; i <= MAX_LINES; i++) {
        if (i == except) continue;
        pthread_mutex_lock(&g_lock);
        bool target = g_online[i].used && g_online[i].id[0];
        pthread_mutex_unlock(&g_lock);
        if (target && notice_push(i, kind, text)) n++;
    }
    return n;
}
