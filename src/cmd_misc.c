/*
 * 利用者・チャット・入会・SYSOP 向けのコマンド
 */
#include "session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 「〜しませんね (y/n)」形式の問い。N と答えたときだけ 1 (空 Enter は実行しない) */
static int yn_neg(struct sess *s, int id) {
    char a[16];
    CHK(ask(s, id, a, sizeof a, RL_UPPER));
    return a[0] == 'N';
}

/* 今の値を見せて聞き直す。空 Enter なら今の値のまま */
static int ask_keep(struct sess *s, int id, char *field, size_t sz, bool show) {
    char a[LINE_MAX_BYTES];
    if (show && field[0]) CHK(out(s, "[%s]\n", field));
    CHK(ask(s, id, a, sizeof a, 0));
    if (a[0]) snprintf(field, sz, "%s", a);
    return 0;
}

/* ------------------------------------------------------------ 会員データの登録 */

int bs_test(struct sess *s) {
    int r = yn(s, 17);
    if (r < 0) return r;
    USER(s)->bs_one_col = r;
    return 0;
}

static bool logname_ok(const char *name, int self) {
    if (!name[0] || utf8_width(name) > 20) return false;
    bool digits = true;
    for (const char *p = name; *p; p++)
        if (!isdigit((unsigned char)*p)) digits = false;
    if (digits || !strcasecmp(name, "GUEST")) return false;
    pthread_mutex_lock(&g_lock);
    const struct user *u = user_find(name);
    bool dup = u && u->id != self;
    pthread_mutex_unlock(&g_lock);
    return !dup;
}

/* 初めてのログイン (initial) か UWRITE で公開情報を入力する */
int user_setup(struct sess *s, bool initial) {
    struct user tmp = *USER(s);
    bool show = !initial;
    if (initial) CHK(outm_nl(s, 8));
    CHK(ask_keep(s, 10, tmp.addr_pub, sizeof tmp.addr_pub, show));
    CHK(ask_keep(s, 11, tmp.job, sizeof tmp.job, show));
    CHK(ask_keep(s, 12, tmp.machine, sizeof tmp.machine, show));
    CHK(ask_keep(s, 14, tmp.birth, sizeof tmp.birth, show));
    int r = yn(s, 15);
    if (r < 0) return r;
    snprintf(tmp.sex, sizeof tmp.sex, "%s", r ? "M" : "F");
    r = yn(s, 16);
    if (r < 0) return r;
    tmp.esc = r;
    r = yn(s, 17);
    if (r < 0) return r;
    tmp.bs_one_col = r;
    for (;;) {
        CHK(ask_keep(s, 9, tmp.logname, sizeof tmp.logname, show));
        if (logname_ok(tmp.logname, s->uid)) break;
        CHK(outm_nl(s, 18));
        tmp.logname[0] = 0;
    }
    CHK(ask_keep(s, 13, tmp.intro, sizeof tmp.intro, show));
    pthread_mutex_lock(&g_lock);
    struct user *u = USER(s);
    snprintf(u->addr_pub, sizeof u->addr_pub, "%s", tmp.addr_pub);
    snprintf(u->job, sizeof u->job, "%s", tmp.job);
    snprintf(u->machine, sizeof u->machine, "%s", tmp.machine);
    snprintf(u->birth, sizeof u->birth, "%s", tmp.birth);
    snprintf(u->sex, sizeof u->sex, "%s", tmp.sex);
    snprintf(u->logname, sizeof u->logname, "%s", tmp.logname);
    snprintf(u->intro, sizeof u->intro, "%s", tmp.intro);
    u->esc = tmp.esc;
    u->bs_one_col = tmp.bs_one_col;
    u->flags &= ~UF_NEW;
    db_save_users();
    char idbuf[16];
    snprintf(idbuf, sizeof idbuf, "%d", u->id);
    pthread_mutex_unlock(&g_lock);
    online_set_user(s->no, idbuf, tmp.logname);
    return 0;
}

/* 入会申し込みの項目。全部入ったら 1、足りなければ 0 */
int application_input(struct sess *s, struct application *a) {
    CHK(ask(s, 19, a->name, sizeof a->name, 0));
    CHK(ask(s, 20, a->kana, sizeof a->kana, 0));
    CHK(ask(s, 21, a->addr, sizeof a->addr, 0));
    CHK(ask(s, 22, a->zip, sizeof a->zip, 0));
    CHK(ask(s, 23, a->tel, sizeof a->tel, 0));
    for (;;) {
        char p2[32];
        CHK(ask(s, 24, a->pass, sizeof a->pass, RL_MASK | RL_UPPER));
        if (!a->pass[0]) break;
        CHK(ask(s, 25, p2, sizeof p2, RL_MASK | RL_UPPER));
        if (!strcmp(a->pass, p2)) break;
    }
    if (!a->name[0] || !a->kana[0] || !a->addr[0] || !a->tel[0] || !a->pass[0]) {
        CHK(outm_nl(s, 28));
        return 0;
    }
    a->t = time(NULL);
    return 1;
}

/* 申し込みから ID を作る。作った ID (失敗は -1) */
static int issue_from(const struct application *a, int level) {
    int id = user_new(level);
    if (id <= 0) return -1;
    struct user *u = &g_users[id];
    snprintf(u->name, sizeof u->name, "%s", a->name);
    snprintf(u->kana, sizeof u->kana, "%s", a->kana);
    snprintf(u->addr_priv, sizeof u->addr_priv, "%s", a->addr);
    snprintf(u->zip, sizeof u->zip, "%s", a->zip);
    snprintf(u->tel, sizeof u->tel, "%s", a->tel);
    snprintf(u->pass, sizeof u->pass, "%s", a->pass);
    u->applied = a->t;
    db_save_users();
    db_save_ptrs();
    return id;
}

/* オンラインで ID を自動発行する (ゲストのログイン時と JOIN) */
int signup_auto(struct sess *s, bool ask_first) {
    if (ask_first) {
        int r = yn(s, 349);
        if (r <= 0) return r;
    }
    struct application a = {0};
    int r = application_input(s, &a);
    if (r <= 0) return r;
    r = yn(s, 350);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    int id = issue_from(&a, g_sys.temp_level);
    pthread_mutex_unlock(&g_lock);
    if (id <= 0) return outm_nl(s, 351);
    nc_log("CH%02d: 仮 ID %d を自動発行", s->no, id);
    CHK(out(s, "｢%s%04d%s\n", g_sys.net_id, id, M(352)));
    char dummy[8];
    return ask(s, 353, dummy, sizeof dummy, 0);
}

int cmd_join(struct sess *s, const char *arg) {
    if (g_sys.signup == SIGNUP_OFFLINE) return outm_nl(s, 348);
    if (g_sys.signup == SIGNUP_AUTO) return signup_auto(s, false);
    struct application a = {0};
    int r = application_input(s, &a);
    if (r <= 0) return r;
    r = yn(s, 26);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    app_add(&a);
    pthread_mutex_unlock(&g_lock);
    nc_log("CH%02d: 入会の申し込み %s", s->no, a.name);
    return outm_nl(s, 27);
}

/* ------------------------------------------------------------ 会員の情報 */

int cmd_idlist(struct sess *s, const char *arg) {
    int from = 0;
    CHK(ask_int(s, 125, 0, &from));
    CHK(outm_nl(s, 122));
    CHK(outm_nl(s, 123));
    int count = 0;
    for (int i = from; i < MAX_USERS; i++) {
        pthread_mutex_lock(&g_lock);
        if (i >= g_nusers) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        const struct user *u = user_get(i);
        char line[128] = "";
        if (u && u->logname[0]) snprintf(line, sizeof line, "%04d:%s\n", u->id, u->logname);
        pthread_mutex_unlock(&g_lock);
        if (!line[0]) continue;
        CHK(out(s, "%s", line));
        int r = paging(s, &count, false);
        if (r) return r < 0 ? r : 0;
    }
    return 0;
}

int cmd_newmem(struct sess *s, const char *arg) {
    CHK(outm_nl(s, 121));
    CHK(outm_nl(s, 122));
    CHK(outm_nl(s, 123));
    /* 発行日の新しい順に 20 人 */
    int ids[20], n = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = g_nusers - 1; i >= 2 && n < 20; i--) {
        const struct user *u = user_get(i);
        if (u && u->logname[0]) ids[n++] = i;
    }
    char buf[20 * 96] = "";
    for (int k = 0; k < n; k++) {
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, "%04d:%s\n", ids[k], g_users[ids[k]].logname);
    }
    pthread_mutex_unlock(&g_lock);
    return out(s, "%s", buf);
}

static int show_user(struct sess *s, const struct user *src) {
    struct user u;
    pthread_mutex_lock(&g_lock);
    u = *src;
    pthread_mutex_unlock(&g_lock);
    char issued[32] = "-", last[32] = "-";
    if (u.issued) fmt_time(u.issued, issued, sizeof issued);
    if (u.last_login) fmt_time(u.last_login, last, sizeof last);
    CHK(out(s, "ID        : %s%04d  %s\n", g_sys.net_id, u.id, level_name(u.level)));
    CHK(out(s, "ログネーム: %s\n性別      : %s\n住所      : %s\n職業      : %s\n機種      : %s\n", u.logname,
            !strcmp(u.sex, "M") ? "男" : !strcmp(u.sex, "F") ? "女" : "-", u.addr_pub, u.job, u.machine));
    CHK(out(s, "自己紹介  : %s\n発行      : %s\n最終ログイン: %s\n書き込み  : ボード %d / メール %d\n", u.intro, issued,
            last, u.wb, u.wm));
    if (IS_SYSOP(s))
        CHK(out(s, "[SYSOP] 氏名 %s (%s)  〒%s %s  TEL %s  生年月日 %s  レベル %d  持ち時間 %d 分\n", u.name, u.kana,
                u.zip, u.addr_priv, u.tel, u.birth, u.level, u.day_minutes));
    return 0;
}

int cmd_uread(struct sess *s, const char *arg) {
    struct user *u = NULL;
    if (arg[0]) {
        pthread_mutex_lock(&g_lock);
        u = user_find(arg);
        pthread_mutex_unlock(&g_lock);
        if (!u) return outm_nl(s, 135);
    } else if (!(u = choose_user(s, 134))) return 0;
    return show_user(s, u);
}

int cmd_uwrite(struct sess *s, const char *arg) { return user_setup(s, false); }

int cmd_mode(struct sess *s, const char *arg) {
    int menu = yn(s, 138);
    if (menu < 0) return menu;
    int chat = yn(s, 140);
    if (chat < 0) return chat;
    int esc = yn(s, 16);
    if (esc < 0) return esc;
    CHK(bs_test(s));
    pthread_mutex_lock(&g_lock);
    USER(s)->menu = menu;
    USER(s)->chat_on_login = chat;
    USER(s)->esc = esc;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

int cmd_pass(struct sess *s, const char *arg) {
    int r = yn(s, 132);
    if (r <= 0) return r;
    char p1[32], p2[32];
    CHK(ask(s, 24, p1, sizeof p1, RL_MASK | RL_UPPER));
    if (!p1[0]) return 0;
    CHK(ask(s, 25, p2, sizeof p2, RL_MASK | RL_UPPER));
    if (strcmp(p1, p2)) return 0;
    pthread_mutex_lock(&g_lock);
    snprintf(USER(s)->pass, sizeof USER(s)->pass, "%s", p1);
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 133);
}

int cmd_ustat(struct sess *s, const char *arg) {
    struct user u;
    pthread_mutex_lock(&g_lock);
    u = *USER(s);
    pthread_mutex_unlock(&g_lock);
    long now_used = (long)(time(NULL) - s->login_at);
    return out(s,
               "ログイン回数   : %d (今月 %d / 先月 %d)\n"
               "利用時間       : 合計 %ld 分 / 今月 %ld 分 / 先月 %ld 分\n"
               "今回の接続     : %ld 分\n"
               "本日の持ち時間 : %d 分 (1 日 %d 分)\n"
               "書き込み       : ボード %d / メール %d\n",
               u.logins, u.logins_month, u.logins_prev, (u.total_sec + now_used) / 60, (u.month_sec + now_used) / 60,
               u.prev_month_sec / 60, now_used / 60, u.today_left, u.day_minutes, u.wb, u.wm);
}

int cmd_log(struct sess *s, const char *arg) {
    struct logent e[40];
    pthread_mutex_lock(&g_lock);
    int n = log_recent(e, 40);
    pthread_mutex_unlock(&g_lock);
    CHK(outm_nl(s, 126));
    CHK(outm_nl(s, 127));
    int count = 0;
    for (int i = n - 1; i >= 0; i--) {
        char d[16], a[8], b[8];
        strftime(d, sizeof d, "%y/%m/%d", localtime(&e[i].in));
        strftime(a, sizeof a, "%H:%M", localtime(&e[i].in));
        strftime(b, sizeof b, "%H:%M", localtime(&e[i].out));
        CHK(out(s, "%s\t%s -%s  %02d-%s\t%04d:%s\n", d, a, b, e[i].line, e[i].speed, e[i].id, e[i].logname));
        int r = paging(s, &count, false);
        if (r) return r < 0 ? r : 0;
    }
    return 0;
}

int cmd_cls(struct sess *s, const char *arg) { return outm_nl(s, 336); }

int cmd_version(struct sess *s, const char *arg) {
    return out(s, "null-net-cock %s (NET-COCK 互換ホスト)\n", NC_VERSION);
}

/* ------------------------------------------------------------ 回線・チャット */

int cmd_llist(struct sess *s, const char *arg) {
    CHK(outm_nl(s, 148));
    CHK(outm_nl(s, 149));
    time_t now = time(NULL);
    for (int i = 1; i <= g_cfg.max_lines; i++) {
        char line[256];
        pthread_mutex_lock(&g_lock);
        struct online o = g_online[i];
        pthread_mutex_unlock(&g_lock);
        if (!o.used || !o.id[0] || (o.secret && !IS_SYSOP(s)))
            snprintf(line, sizeof line, "%2d.\t%s\n", i, M(144));
        else {
            long m = (long)(now - o.since) / 60;
            snprintf(line, sizeof line, "%2d.\t%-8s %s\t%3ld:%02ld\t%04d:%s\n", i, o.place, o.chat ? "C" : "-",
                     m / 60, m % 60, atoi(o.id), o.handle);
        }
        CHK(out(s, "%s", line));
    }
    return 0;
}

static int set_chat(struct sess *s, bool on) {
    pthread_mutex_lock(&g_lock);
    g_online[s->no].chat = on;
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, on ? 151 : 150);
}

int cmd_coff(struct sess *s, const char *arg) { return set_chat(s, false); }
int cmd_con(struct sess *s, const char *arg) { return set_chat(s, true); }

/* チャット。引数に回線番号を付けるとその回線への電報 */
int cmd_chat(struct sess *s, const char *arg) {
    char name[80];
    snprintf(name, sizeof name, "%02d:%s", s->no, USER(s)->logname);
    if (isdigit((unsigned char)arg[0])) {
        int to = atoi(arg);
        char text[400], msg[512];
        const char *p = strchr(arg, ' ');
        if (p) snprintf(text, sizeof text, "%s", p + 1);
        else CHK(ask(s, 156, text, sizeof text, 0));
        if (!text[0]) return 0;
        pthread_mutex_lock(&g_lock);
        bool ok = to >= 1 && to <= MAX_LINES && g_online[to].used && g_online[to].id[0];
        pthread_mutex_unlock(&g_lock);
        if (!ok) return outm_nl(s, 144);
        snprintf(msg, sizeof msg, "<<電報 %s>> %s", name, text);
        notice_push(to, N_TELEGRAM, msg);
        return 0;
    }
    bool was = g_online[s->no].chat;
    g_online[s->no].chat = true;
    CHK(outm_nl(s, 162));
    char msg[512];
    snprintf(msg, sizeof msg, "[%s] チャットに入りました", name);
    for (int i = 1; i <= MAX_LINES; i++)
        if (i != s->no && g_online[i].used && g_online[i].id[0] && g_online[i].chat) notice_push(i, N_CHAT, msg);
    int r = 0;
    for (;;) {
        char text[400];
        if ((r = ask(s, 156, text, sizeof text, 0)) < 0) break;
        if (!text[0]) continue;
        if ((text[0] == 'Q' || text[0] == 'q') && !text[1]) {
            r = outm_nl(s, 157);
            break;
        }
        if (!strcmp(text, "?")) {
            if ((r = out(s, "%s\n", M(162))) < 0) break;
            continue;
        }
        snprintf(msg, sizeof msg, "[%s] %s", name, text);
        for (int i = 1; i <= MAX_LINES; i++)
            if (i != s->no && g_online[i].used && g_online[i].id[0] && g_online[i].chat) notice_push(i, N_CHAT, msg);
    }
    g_online[s->no].chat = was;
    return r < 0 ? r : 0;
}

/* ------------------------------------------------------------ SYSOP: 会員 */

int cmd_makeid(struct sess *s, const char *arg) {
    for (;;) {
        int n, idx = -1;
        pthread_mutex_lock(&g_lock);
        struct application *list = app_list(&n);
        for (int i = 0; i < n; i++)
            if (!list[i].issued_id) {
                idx = i;
                break;
            }
        struct application a = idx >= 0 ? list[idx] : (struct application){0};
        pthread_mutex_unlock(&g_lock);
        if (idx < 0) return outm_nl(s, 29);
        char ts[32];
        fmt_time(a.t, ts, sizeof ts);
        CHK(out(s, "\n#%d %s\n氏名 %s (%s)\n〒%s %s\nTEL %s\n", a.no, ts, a.name, a.kana, a.zip, a.addr, a.tel));
        char c[16];
        CHK(ask(s, 31, c, sizeof c, RL_UPPER));
        if (c[0] == 'Q') return outm_nl(s, 30);
        pthread_mutex_lock(&g_lock);
        list = app_list(&n);
        int id = 0;
        if (c[0] == 'Y') {
            id = issue_from(&a, g_sys.user_level);
            list[idx].issued_id = id > 0 ? id : -1;
        } else list[idx].issued_id = -1; /* 見送り */
        app_save();
        pthread_mutex_unlock(&g_lock);
        if (c[0] == 'Y' && id <= 0) return outm_nl(s, 34);
        if (id > 0) CHK(out(s, "%s%04d %s\n", g_sys.net_id, id, M(32)));
        else CHK(outm_nl(s, 33));
    }
}

int cmd_gulv(struct sess *s, const char *arg) {
    for (int i = 2; i < MAX_USERS; i++) {
        pthread_mutex_lock(&g_lock);
        if (i >= g_nusers) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        struct user *u = user_get(i);
        bool target = u && u->level > 0 && u->level < g_sys.user_level;
        pthread_mutex_unlock(&g_lock);
        if (!target) continue;
        CHK(show_user(s, u));
        char c[16];
        CHK(ask(s, 358, c, sizeof c, RL_UPPER));
        if (c[0] == 'Q') return 0;
        pthread_mutex_lock(&g_lock);
        if (c[0] == 'Y') u->level = g_sys.user_level;
        if (c[0] == 'D') u->flags |= UF_DELETED;
        db_save_users();
        pthread_mutex_unlock(&g_lock);
        if (c[0] == 'Y') CHK(outm_nl(s, 359));
        if (c[0] == 'D') CHK(outm_nl(s, 204));
    }
    return 0;
}

int cmd_ulevel(struct sess *s, const char *arg) {
    struct user *u = choose_user(s, 216);
    if (!u) return 0;
    CHK(out(s, "%04d:%s  %d\n", u->id, u->logname, u->level));
    int v;
    CHK(ask_int(s, 153, u->level, &v));
    if (v < 0 || v > 255) return 0;
    pthread_mutex_lock(&g_lock);
    u->level = v;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

int cmd_utime(struct sess *s, const char *arg) {
    struct user *u = choose_user(s, 216);
    if (!u) return 0;
    CHK(out(s, "%04d:%s  %d\n", u->id, u->logname, u->day_minutes));
    int v;
    CHK(ask_int(s, 154, u->day_minutes, &v));
    if (v < 0 || v > 1440) return 0;
    pthread_mutex_lock(&g_lock);
    u->day_minutes = u->today_left = v;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

int cmd_mchange(struct sess *s, const char *arg) {
    struct user *u = choose_user(s, 216);
    if (!u) return 0;
    struct user tmp;
    pthread_mutex_lock(&g_lock);
    tmp = *u;
    pthread_mutex_unlock(&g_lock);
    CHK(ask_keep(s, 19, tmp.name, sizeof tmp.name, true));
    CHK(ask_keep(s, 20, tmp.kana, sizeof tmp.kana, true));
    CHK(ask_keep(s, 21, tmp.addr_priv, sizeof tmp.addr_priv, true));
    CHK(ask_keep(s, 22, tmp.zip, sizeof tmp.zip, true));
    CHK(ask_keep(s, 23, tmp.tel, sizeof tmp.tel, true));
    CHK(ask_keep(s, 14, tmp.birth, sizeof tmp.birth, true));
    pthread_mutex_lock(&g_lock);
    memcpy(u->name, tmp.name, sizeof u->name);
    memcpy(u->kana, tmp.kana, sizeof u->kana);
    memcpy(u->addr_priv, tmp.addr_priv, sizeof u->addr_priv);
    memcpy(u->zip, tmp.zip, sizeof u->zip);
    memcpy(u->tel, tmp.tel, sizeof u->tel);
    memcpy(u->birth, tmp.birth, sizeof u->birth);
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

int cmd_idkill(struct sess *s, const char *arg) {
    struct user *u = choose_user(s, 205);
    if (!u) return 0;
    if (u->id <= 1 || u->id == s->uid) return out(s, "==== この ID は削除できません ====\n");
    CHK(out(s, "%04d:%s\n", u->id, u->logname));
    int r = yn_neg(s, 203);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    u->flags |= UF_DELETED;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    nc_log("CH%02d: ID %d を削除", s->no, u->id);
    return outm_nl(s, 204);
}

int cmd_idset(struct sess *s, const char *arg) {
    char a[64];
    CHK(ask(s, 216, a, sizeof a, 0));
    if (!a[0]) return 0;
    int id = atoi(a);
    if (id <= 1 || id >= MAX_USERS) return outm_nl(s, 135);
    static const struct { int msg, flag; } q[] = {{227, UF_GUEST}, {228, UF_DELETED}, {229, UF_NEW}};
    pthread_mutex_lock(&g_lock);
    struct user *u = &g_users[id];
    bool exists = id < g_nusers && u->id == id;
    pthread_mutex_unlock(&g_lock);
    if (!exists) return outm_nl(s, 135);
    int flags = u->flags;
    for (size_t i = 0; i < sizeof q / sizeof q[0]; i++) {
        char c[16];
        CHK(ask(s, q[i].msg, c, sizeof c, RL_UPPER));
        if (c[0] == 'N') flags |= q[i].flag;
        else if (c[0] == 'Y') flags &= ~q[i].flag;
    }
    pthread_mutex_lock(&g_lock);
    u->flags = flags;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

int cmd_upass(struct sess *s, const char *arg) {
    struct user *u = choose_user(s, 247);
    if (!u) return 0;
    CHK(out(s, "%s%s\n%s%s\n", M(250), u->logname, M(248), u->pass));
    int r = yn_neg(s, 249);
    if (r <= 0) return r;
    char p[32];
    CHK(ask(s, 24, p, sizeof p, RL_UPPER));
    if (!p[0]) return 0;
    pthread_mutex_lock(&g_lock);
    snprintf(u->pass, sizeof u->pass, "%s", p);
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 133);
}

/* ------------------------------------------------------------ SYSOP: ボード */

/* y/n の問い。空 Enter は今の値のまま。neg は「〜しませんね」形式 (N で真) */
static int yn_keep(struct sess *s, int id, bool cur, bool neg) {
    char a[16];
    CHK(out(s, "[%c] ", (cur != neg) ? 'Y' : 'N'));
    CHK(ask(s, id, a, sizeof a, RL_UPPER));
    if (a[0] == 'Y') return !neg;
    if (a[0] == 'N') return neg;
    return cur;
}

/* ボードの属性を聞く (BMAKE と BCHANGE)。空 Enter は今の値のまま。1: 入力した 0: やめた */
static int board_input(struct sess *s, struct board *b) {
    CHK(ask_int(s, 166, b->bop, &b->bop));
    CHK(ask_int(s, 167, b->rlevel, &b->rlevel));
    CHK(ask_int(s, 168, b->wlevel, &b->wlevel));
    CHK(ask_int(s, 169, b->group, &b->group));
    CHK(ask_int(s, 170, b->age_min, &b->age_min));
    CHK(ask_int(s, 171, b->age_max, &b->age_max));
    CHK(ask_int(s, 172, b->sex, &b->sex));
    CHK(ask_int(s, 173, b->keep, &b->keep));
    if (b->no != MAIL_BOARD) {
        char a[16];
        CHK(out(s, "[%s] ", b->type == BT_PROGRAM ? "program" : "board"));
        CHK(ask(s, 174, a, sizeof a, RL_UPPER));
        if (a[0] == 'P') b->type = BT_PROGRAM;
        else if (a[0] == 'B') b->type = BT_BOARD;
    }
    int r = yn_keep(s, 175, b->esc, false);
    if (r < 0) return r;
    b->esc = r;
    r = yn_keep(s, 176, b->cug, true);
    if (r < 0) return r;
    b->cug = r;
    if (b->no == MAIL_BOARD) return 1;
    for (;;) {
        char idx[16];
        if (b->index[0]) CHK(out(s, "[%s] ", b->index));
        CHK(ask(s, 177, idx, sizeof idx, RL_UPPER));
        if (!idx[0]) {
            if (!b->index[0]) return 0;
            break;
        }
        pthread_mutex_lock(&g_lock);
        bool dup = false;
        for (int i = 1; i < MAX_BOARDS; i++)
            if (i != b->no && g_boards[i].used && !strcasecmp(g_boards[i].index, idx)) dup = true;
        pthread_mutex_unlock(&g_lock);
        if (!dup && !isdigit((unsigned char)idx[0])) {
            snprintf(b->index, sizeof b->index, "%s", idx);
            break;
        }
        CHK(outm_nl(s, 165));
    }
    char title[128];
    if (b->title[0]) CHK(out(s, "[%s]\n", b->title));
    CHK(ask(s, 178, title, sizeof title, 0));
    if (title[0]) snprintf(b->title, sizeof b->title, "%s", title);
    return 1;
}

int cmd_bmake(struct sess *s, const char *arg) {
    int r = yn(s, 179);
    if (r < 0) return r;
    if (r) print_board_list(s, false);
    int no;
    CHK(ask_int(s, 164, -1, &no));
    pthread_mutex_lock(&g_lock);
    bool bad = no < 1 || no >= MAX_BOARDS || g_boards[no].used;
    pthread_mutex_unlock(&g_lock);
    if (bad) return outm_nl(s, 165);
    struct board b = {.used = true, .no = no, .type = BT_BOARD, .next_seq = 1, .bop = s->uid,
                      .wlevel = g_sys.temp_level};
    r = board_input(s, &b);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    g_boards[no] = b;
    db_save_boards();
    pthread_mutex_unlock(&g_lock);
    nc_log("CH%02d: ボード %d (%s) を作成", s->no, no, b.index);
    return outm_nl(s, 220);
}

/* ボードの属性を変える (0 番のメールも選べる) */
int cmd_bchange(struct sess *s, const char *arg) {
    int r = yn(s, 179);
    if (r < 0) return r;
    if (r) print_board_list(s, false);
    char a[32];
    CHK(ask(s, 180, a, sizeof a, RL_UPPER));
    if (!a[0]) return 0;
    pthread_mutex_lock(&g_lock);
    const char *key = a[0] == '\\' ? a + 1 : a;
    struct board *bp = !strcmp(key, "0") ? &g_boards[MAIL_BOARD] : board_by_index(key);
    struct board b = bp ? *bp : (struct board){0};
    pthread_mutex_unlock(&g_lock);
    if (!bp) return outm_nl(s, 335);
    r = board_input(s, &b);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    if (g_boards[b.no].used) {
        b.next_seq = g_boards[b.no].next_seq;
        g_boards[b.no] = b;
    }
    db_save_boards();
    pthread_mutex_unlock(&g_lock);
    nc_log("CH%02d: ボード %d (%s) の設定を変更", s->no, b.no, b.index);
    return outm_nl(s, 220);
}

int cmd_bkill(struct sess *s, const char *arg) {
    int b = choose_board(s, 368, false);
    if (b < -1) return b + 1;
    if (b < 0) return 0;
    int r = yn_neg(s, 369);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmsgs; i++)
        if (g_msgs[i].board == b) g_msgs[i].deleted = true;
    g_boards[b].used = false;
    db_save_boards();
    db_save_msgs();
    pthread_mutex_unlock(&g_lock);
    CHK(outm_nl(s, 370));
    return outm_nl(s, 360);
}

/* SYSOP かそのボードのボードオペなら 1 */
static bool can_manage(struct sess *s, int b) { return IS_SYSOP(s) || g_boards[b].bop == s->uid; }

int cmd_btitle(struct sess *s, const char *arg) {
    int b = choose_board(s, 180, false);
    if (b < -1) return b + 1;
    if (b < 0) return 0;
    if (!can_manage(s, b)) return outm_nl(s, 145);
    char title[128];
    CHK(out(s, "[%s]\n", g_boards[b].title));
    CHK(ask(s, 178, title, sizeof title, 0));
    if (!title[0]) return 0;
    pthread_mutex_lock(&g_lock);
    snprintf(g_boards[b].title, sizeof g_boards[b].title, "%s", title);
    db_save_boards();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

/* CUG のアクセス権 */
int cmd_buser(struct sess *s, const char *arg) {
    int b = choose_board(s, 180, false);
    if (b < -1) return b + 1;
    if (b < 0) return 0;
    if (!can_manage(s, b)) return outm_nl(s, 145);
    for (;;) {
        struct user *u = choose_user(s, 216);
        if (!u) return 0;
        CHK(out(s, "%04d:%s [%c]\n", u->id, u->logname, *cug_right(u->id, b)));
        char a[16];
        CHK(ask(s, 219, a, sizeof a, 0));
        char c = (char)tolower((unsigned char)a[0]);
        if (!strchr("arwn", c) || !c) continue;
        pthread_mutex_lock(&g_lock);
        *cug_right(u->id, b) = c;
        db_save_ptrs();
        pthread_mutex_unlock(&g_lock);
        CHK(outm_nl(s, 220));
    }
}

/* ------------------------------------------------------------ SYSOP: システム */

int cmd_mesedit(struct sess *s, const char *arg) {
    int n;
    CHK(ask_int(s, 163, -1, &n));
    if (n < 0 || n > 9) return 0;
    CHK(out(s, "%s\n----\n", g_sys.sysmes[n]));
    CHK(outm_nl(s, 192));
    char text[1024] = "";
    for (int l = 0; l < 10; l++) {
        char line[128];
        CHK(term_readline(s->t, "", line, sizeof line, RL_RAW));
        if (!strcmp(line, ".")) break;
        size_t len = strlen(text);
        snprintf(text + len, sizeof text - len, "%s%s", len ? "\n" : "", line);
    }
    int r = yn(s, 194);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    snprintf(g_sys.sysmes[n], sizeof g_sys.sysmes[n], "%s", text);
    db_save_sys();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

int cmd_store(struct sess *s, const char *arg) {
    pthread_mutex_lock(&g_lock);
    db_save_all();
    pthread_mutex_unlock(&g_lock);
    return out(s, "==== 保存しました ====\n");
}

static int set_secret(struct sess *s, bool on) {
    pthread_mutex_lock(&g_lock);
    USER(s)->secret = on;
    g_online[s->no].secret = on;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, on ? 212 : 213);
}
int cmd_secret(struct sess *s, const char *arg) { return set_secret(s, true); }
int cmd_secoff(struct sess *s, const char *arg) { return set_secret(s, false); }

static int ask_num(struct sess *s, const char *label, int id, int *v, int max) {
    CHK(out(s, "%s [%d] ", label, *v));
    int n;
    CHK(ask_int(s, id, *v, &n));
    if (n >= 0 && n <= max) *v = n;
    return 0;
}

int cmd_sysset(struct sess *s, const char *arg) {
    struct system t;
    pthread_mutex_lock(&g_lock);
    t = g_sys;
    pthread_mutex_unlock(&g_lock);
    char id[16];
    CHK(out(s, "ネットワーク ID [%s] ", t.net_id));
    CHK(ask(s, 339, id, sizeof id, RL_UPPER));
    if (id[0]) snprintf(t.net_id, sizeof t.net_id, "%.4s", id);
    CHK(ask_num(s, "一般会員のレベル", 340, &t.user_level, 255));
    CHK(ask_num(s, "仮 ID のレベル", 340, &t.temp_level, 255));
    CHK(ask_num(s, "チャットできるレベル", 340, &t.chat_level, 255));
    CHK(ask_num(s, "ゲストの持ち時間", 154, &t.min_minutes, 1440));
    CHK(ask_num(s, "会員の持ち時間", 154, &t.user_minutes, 1440));
    CHK(ask_num(s, "メールの大きさ", 341, &t.mail_size, 65536));
    CHK(ask_num(s, "会員管理者の ID", 343, &t.manager, MAX_USERS - 1));
    pthread_mutex_lock(&g_lock);
    memcpy(g_sys.net_id, t.net_id, sizeof g_sys.net_id);
    g_sys.user_level = t.user_level;
    g_sys.temp_level = t.temp_level;
    g_sys.chat_level = t.chat_level;
    g_sys.min_minutes = t.min_minutes;
    g_sys.user_minutes = t.user_minutes;
    g_sys.mail_size = t.mail_size;
    g_sys.manager = t.manager;
    db_save_sys();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

int cmd_signup(struct sess *s, const char *arg) {
    int r = yn(s, 344);
    if (r < 0) return r;
    int mode;
    if (!r) mode = SIGNUP_OFFLINE;
    else {
        r = yn(s, 345);
        if (r < 0) return r;
        mode = r ? SIGNUP_MANUAL : SIGNUP_AUTO;
    }
    pthread_mutex_lock(&g_lock);
    g_sys.signup = mode;
    db_save_sys();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, mode == SIGNUP_OFFLINE ? 348 : mode == SIGNUP_MANUAL ? 346 : 347);
}

static int set_aoff(struct sess *s, bool off) {
    pthread_mutex_lock(&g_lock);
    g_sys.aoff = off;
    db_save_sys();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, off ? 210 : 211);
}
int cmd_aon(struct sess *s, const char *arg) { return set_aoff(s, false); }
int cmd_aoff(struct sess *s, const char *arg) { return set_aoff(s, true); }

int cmd_filem(struct sess *s, const char *arg) {
    int r = yn_neg(s, 295);
    if (r <= 0) return r;
    CHK(outm_nl(s, 296));
    pthread_mutex_lock(&g_lock);
    int n = filem();
    pthread_mutex_unlock(&g_lock);
    nc_log("CH%02d: ファイルメンテナンス (%d 件を削除)", s->no, n);
    return out(s, "%s (%d)\n", M(297), n);
}

int cmd_report(struct sess *s, const char *arg) {
    pthread_mutex_lock(&g_lock);
    int napps, pending = 0, users = 0;
    struct application *a = app_list(&napps);
    for (int i = 0; i < napps; i++)
        if (!a[i].issued_id) pending++;
    for (int i = 0; i < g_nusers; i++)
        if (user_get(i)) users++;
    int msgs = 0;
    for (int i = 0; i < g_nmsgs; i++)
        if (!g_msgs[i].deleted) msgs++;
    struct system t = g_sys;
    pthread_mutex_unlock(&g_lock);
    char started[32], now[32];
    fmt_time(t.started, started, sizeof started);
    fmt_time(time(NULL), now, sizeof now);
    return out(s, "%s%s\n%s%d\n%s%d/%d\n%s%d\n%s%ld\n%s%ld (ゲスト %ld)\n%s%s\n", M(389), started, M(271), pending,
               M(272), users, MAX_USERS, M(273), msgs, M(274), (long)(time(NULL) - t.started) / 60, M(275),
               t.total_logins, t.guest_logins, M(281), now);
}
