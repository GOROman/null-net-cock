/*
 * セッション: ログイン、コマンドの受け付け、共通の入出力部品
 */
#include "session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------ 入出力の部品 */

int out(struct sess *s, const char *fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return term_print(s->t, buf);
}

int outm(struct sess *s, int id) { return term_print(s->t, M(id)); }

int outm_nl(struct sess *s, int id) {
    CHK(term_print(s->t, M(id)));
    return term_print(s->t, "\n");
}

int ask(struct sess *s, int id, char *buf, size_t sz, int flags) { return term_readline(s->t, M(id), buf, sz, flags); }

int ask_str(struct sess *s, const char *prompt, char *buf, size_t sz, int flags) {
    return term_readline(s->t, prompt, buf, sz, flags);
}

int yn(struct sess *s, int id) {
    char a[16];
    CHK(term_readline(s->t, M(id), a, sizeof a, RL_UPPER));
    return a[0] == 'Y';
}

int ask_int(struct sess *s, int id, int def, int *val) {
    char a[32];
    CHK(ask(s, id, a, sizeof a, 0));
    *val = a[0] ? atoi(a) : def;
    return 0;
}

/* 1 行表示するごとに呼ぶ。1 画面ぶんたまったら「まだ続けますか」。1: やめる */
int paging(struct sess *s, int *count, bool continuous) {
    if (continuous) return 0;
    if (++*count < s->t->rows - 2) return 0;
    *count = 0;
    int r = yn(s, 131);
    if (r < 0) return r;
    return r ? 0 : 1;
}

struct user *choose_user(struct sess *s, int prompt_id) {
    char a[64];
    if (ask(s, prompt_id, a, sizeof a, 0) < 0 || !a[0]) return NULL;
    pthread_mutex_lock(&g_lock);
    struct user *u = user_find(a);
    pthread_mutex_unlock(&g_lock);
    if (!u) outm_nl(s, 135);
    return u;
}

int choose_board(struct sess *s, int prompt_id, bool for_write) {
    for (;;) {
        char a[32];
        int r = ask(s, prompt_id, a, sizeof a, RL_UPPER);
        if (r < 0) return r - 1;
        if (!a[0]) return -1;
        if (a[0] == '?') {
            print_board_list(s, for_write);
            continue;
        }
        const char *key = a[0] == '\\' ? a + 1 : a;
        pthread_mutex_lock(&g_lock);
        struct board *b = board_by_index(key);
        bool ok = b && (for_write ? board_can_write(USER(s), b) : board_can_read(USER(s), b));
        int n = b ? b->no : -1;
        pthread_mutex_unlock(&g_lock);
        if (!b) {
            CHK(outm_nl(s, 335) - 1);
            continue;
        }
        if (!ok) {
            CHK(outm_nl(s, for_write ? 183 : 392) - 1);
            continue;
        }
        return n;
    }
}

/* ------------------------------------------------------------ コマンドの表 */

typedef int (*cmd_fn)(struct sess *, const char *);

/* 表の順番が、省略したときの優先順位 (前方一致で最初に当たったもの) */
static const struct command {
    const char *name;
    int level;              /* -1: チャットレベル */
    int desc;               /* 説明のメッセージ ID */
    cmd_fn fn;
    bool manager;           /* 会員管理者だけ */
} commands[] = {
    {"OFF", 0, 74, NULL, false},
    {"BREAD", 0, 53, cmd_bread, false},
    {"BWRITE", 0, 54, cmd_bwrite, false},
    {"BATCH", 0, 58, cmd_batch, false},
    {"BSET", 30, 60, cmd_bset, false},
    {"BTITLE", 80, 76, cmd_btitle, false},
    {"BUSER", 80, 77, cmd_buser, false},
    {"BMAKE", 100, 83, cmd_bmake, false},
    {"BKILL", 100, 84, cmd_bkill, false},
    {"BCHANGE", 100, 85, cmd_bchange, false},
    {"RALL", 30, 61, cmd_rall, false},
    {"RNALL", 30, 62, cmd_rnall, false},
    {"IDLIST", 0, 73, cmd_idlist, false},
    {"IDSET", 100, 97, cmd_idset, false},
    {"IDKILL", 100, 89, cmd_idkill, false},
    {"IDCOPY", 100, 90, cmd_idcopy, false},
    {"UREAD", 40, 67, cmd_uread, false},
    {"UWRITE", 30, 69, cmd_uwrite, false},
    {"UDLIST", 100, 87, cmd_udlist, true},
    {"UDEDIT", 100, 105, cmd_udedit, true},
    {"ULEVEL", 100, 78, cmd_ulevel, false},
    {"UTIME", 100, 79, cmd_utime, false},
    {"UPASS", 100, 100, cmd_upass, false},
    {"USTAT", 30, 112, cmd_ustat, false},
    {"LOG", 0, 64, cmd_log, false},
    {"LLIST", -1, 66, cmd_llist, false},
    {"LCSET", 100, 115, cmd_lcset, false},
    {"LBUSY", 100, 119, cmd_lbusy, false},
    {"LINESET", 100, 110, cmd_lineset, false},
    {"MODE", 30, 68, cmd_mode, false},
    {"MREAD", 30, 56, cmd_mread, false},
    {"MWRITE", 30, 57, cmd_mwrite, false},
    {"MCHECK", 30, 59, cmd_mcheck, false},
    {"MBSET", 30, 99, cmd_mbset, false},
    {"MAKEID", 100, 75, cmd_makeid, true},
    {"MCHANGE", 100, 80, cmd_mchange, true},
    {"MESEDIT", 100, 86, cmd_mesedit, false},
    {"PASS", 30, 70, cmd_pass, false},
    {"PMOVE", 30, 63, cmd_pmove, false},
    {"CLS", 0, 107, cmd_cls, false},
    {"CHAT", -1, 65, cmd_chat, false},
    {"COFF", -1, 81, cmd_coff, false},
    {"CON", -1, 82, cmd_con, false},
    {"CMODE", 100, 96, cmd_cmode, false},
    {"CTIME", 100, 92, cmd_ctime, false},
    {"VERSION", 0, 113, cmd_version, false},
    {"ACCESS", 0, 91, cmd_access, false},
    {"AON", 100, 117, cmd_aon, false},
    {"AOFF", 100, 118, cmd_aoff, false},
    {"ALARM", 100, 106, NULL, false},
    {"JOIN", 0, 71, cmd_join, false},
    {"NEWMEM", 0, 72, cmd_newmem, false},
    {"GULV", 100, 55, cmd_gulv, true},
    {"SECRET", 100, 93, cmd_secret, false},
    {"SECOFF", 100, 94, cmd_secoff, false},
    {"SCSET", 100, 101, cmd_scset, false},
    {"SCLIST", 100, 102, cmd_sclist, false},
    {"STORE", 100, 88, cmd_store, false},
    {"SDTIME", 100, 103, cmd_sdtime, false},
    {"SYSSET", 100, 109, cmd_sysset, false},
    {"SIGNUP", 100, 111, cmd_signup, false},
    {"IMODE", 100, 114, cmd_imode, false},
    {"FILEM", 100, 98, cmd_filem, false},
    {"HFCONT", 100, 95, NULL, false},
    {"REPORT", 100, 104, cmd_report, false},
    {"KEYLOCK", 100, 116, NULL, false},
    {"DEBUG", 100, 108, NULL, true},
};
#define NCMD (int)(sizeof commands / sizeof commands[0])

static bool can_use(struct sess *s, const struct command *c) {
    int need = c->level < 0 ? g_sys.chat_level : c->level;
    /* ゲストは入会の申し込み関係と閲覧だけ */
    if (IS_GUEST(s) && need > 0) return false;
    if (USER(s)->level < need) return false;
    if (c->manager && !IS_MANAGER(s)) return false;
    return true;
}

static int help(struct sess *s) {
    CHK(out(s, "%s\n", M(143)));
    int count = 0;
    for (int i = 0; i < NCMD; i++) {
        if (!can_use(s, &commands[i])) continue;
        CHK(out(s, "  %-8s %s\n", commands[i].name, M(commands[i].desc)));
        int r = paging(s, &count, false);
        if (r) return r < 0 ? r : 0;
    }
    return 0;
}

/* ------------------------------------------------------------ ログイン */

static void notify_others(struct sess *s, int msg_id) {
    struct user *u = USER(s);
    if (u->secret) return;
    char text[256];
    snprintf(text, sizeof text, "%04d:%s %s", u->id, u->logname, M(msg_id));
    notice_broadcast(N_SYSTEM, text, s->no);
}

/* ID とパスワードを聞く。成功したら s->uid に入れて 0 */
static int login(struct sess *s) {
    CHK(out(s, "\n%s\n", g_sys.sysmes[0]));
    for (int tries = 0; tries < 3;) {
        char id[32], pw[32];
        CHK(ask(s, 0, id, sizeof id, RL_UPPER));
        if (!id[0]) continue;
        if (!strcmp(id, "GUEST") || !strcmp(id, "0")) {
            pthread_mutex_lock(&g_lock);
            int need = g_sys.lines[s->no].level;
            bool aoff = g_sys.aoff;
            pthread_mutex_unlock(&g_lock);
            if (aoff) {
                outm_nl(s, 5);
                return T_DISCONNECT;
            }
            if (need > 0) {
                outm_nl(s, 4);
                return T_DISCONNECT;
            }
            s->uid = 0;
            return 0;
        }
        CHK(ask(s, 1, pw, sizeof pw, RL_MASK));
        pthread_mutex_lock(&g_lock);
        struct user *u = user_find(id);
        int result = 0;
        if (!u || u->id == 0) result = 2;
        else if (strcasecmp(u->pass, pw) != 0) {
            u->pass_miss++;
            db_save_users();
            result = 3;
        } else if (g_sys.aoff && u->level < LV_SYSOP) result = 5;
        else if (u->level < g_sys.lines[s->no].level) result = 4;
        else {
            char idbuf[16];
            snprintf(idbuf, sizeof idbuf, "%d", u->id);
            for (int i = 1; i <= MAX_LINES; i++)
                if (i != s->no && g_online[i].used && !strcmp(g_online[i].id, idbuf)) result = 7;
        }
        int uid = u ? u->id : -1;
        pthread_mutex_unlock(&g_lock);
        if (result == 0) {
            s->uid = uid;
            return 0;
        }
        CHK(outm_nl(s, result));
        if (result == 4 || result == 5 || result == 7) return T_DISCONNECT;
        tries++;
    }
    outm_nl(s, 6);
    return T_DISCONNECT;
}

/* ログイン後の表示 */
static int opening(struct sess *s) {
    struct user *u = USER(s);
    CHK(out(s, "\n%s\n", g_sys.sysmes[1]));
    if (IS_GUEST(s)) {
        CHK(out(s, "%s\n%s\n", g_sys.sysmes[3], g_sys.sysmes[5]));
        return 0;
    }
    CHK(out(s, "%s\n", g_sys.sysmes[2]));
    char prev[32] = "-";
    if (u->prev_login) fmt_time(u->prev_login, prev, sizeof prev);
    CHK(out(s, "\n%04d:%s  前回 %s  %d 回目\n", u->id, u->logname, prev, u->logins));
    if (u->pass_miss > 0) {
        CHK(out(s, "パスワードの入力ミスが %d 回ありました。\n", u->pass_miss));
        pthread_mutex_lock(&g_lock);
        u->pass_miss = 0;
        db_save_users();
        pthread_mutex_unlock(&g_lock);
    }
    CHK(login_mail_notice(s));
    CHK(sched_opening(s));
    pthread_mutex_lock(&g_lock);
    bool chat = g_online[s->no].chat;
    int nmsgs = g_nmsgs;
    pthread_mutex_unlock(&g_lock);
    CHK(outm_nl(s, chat ? 151 : 150));
    /* タイトルが多くなったら SYSOP にファイルメンテナンスを促す */
    if (IS_SYSOP(s) && nmsgs > 3500) CHK(outm_nl(s, 360));
    return 0;
}

/* ------------------------------------------------------------ コマンドループ */

/* 1 つのコマンドを実行する。OFF で回線を切るなら 1 */
static int run_command(struct sess *s, const struct command *c, const char *arg) {
    online_set_place(s->no, c->name);
    if (!strcmp(c->name, "OFF")) {
        int r = yn(s, 124);
        return r < 0 ? r : r ? 1 : 0;
    }
    if (!c->fn) return outm_nl(s, 155);
    int r = c->fn(s, arg);
    return r < 0 ? r : 0;
}

static const struct command *find_command(struct sess *s, const char *cmd) {
    size_t n = strlen(cmd);
    for (int i = 0; i < NCMD; i++)
        if (strncmp(commands[i].name, cmd, n) == 0 && can_use(s, &commands[i])) return &commands[i];
    return NULL;
}

/* ------------------------------------------------------------ メニュー方式 */

/* グループ 1〜8 の中身 (NET-COCK のコマンド一覧の並び) */
static const char *const menu_groups[8][16] = {
    {"LOG", "UREAD", "MODE", "UWRITE", "PASS", "IDLIST", "CLS", "USTAT", "VERSION"},
    {"BREAD", "BWRITE", "BATCH", "BSET", "RALL", "RNALL", "PMOVE"},
    {"MREAD", "MWRITE", "MCHECK", "MBSET"},
    {"MESEDIT", "SCSET", "SCLIST", "STORE", "SECRET", "SECOFF", "CMODE", "SDTIME", "ALARM", "SYSSET", "LINESET",
     "IMODE", "AON", "AOFF", "LBUSY"},
    {"BTITLE", "BUSER", "BMAKE", "BKILL", "BCHANGE"},
    {"CHAT", "LLIST", "COFF", "CON", "ACCESS"},
    {"GULV", "JOIN", "NEWMEM", "MAKEID", "ULEVEL", "UTIME", "MCHANGE", "UDLIST", "IDKILL", "IDCOPY", "IDSET", "UPASS",
     "UDEDIT", "SIGNUP", "LCSET"},
    {"CTIME", "HFCONT", "FILEM", "REPORT", "KEYLOCK", "DEBUG"},
};

static const struct command *by_name(const char *name) {
    for (int i = 0; i < NCMD; i++)
        if (!strcmp(commands[i].name, name)) return &commands[i];
    return NULL;
}

/* グループの中で使えるコマンドを番号順に集める */
static int group_items(struct sess *s, int g, const struct command **items) {
    int n = 0;
    for (int k = 0; k < 16 && menu_groups[g - 1][k]; k++) {
        const struct command *c = by_name(menu_groups[g - 1][k]);
        if (c && can_use(s, c)) items[n++] = c;
    }
    return n;
}

static int show_menu(struct sess *s, int g) {
    if (g == 0) {
        CHK(out(s, "\n%s\n", M(44)));
        for (int i = 1; i <= 8; i++) {
            const struct command *items[16];
            if (group_items(s, i, items)) CHK(out(s, " %d. %s\n", i, M(35 + i)));
        }
        return out(s, " 9. %s\n", M(74));
    }
    const struct command *items[16];
    int n = group_items(s, g, items);
    CHK(out(s, "\n%s\n", M(44 + g)));
    for (int i = 0; i < n; i++) CHK(out(s, " %d. %-8s %s\n", i + 1, items[i]->name, M(items[i]->desc)));
    return out(s, " 0. %s\n", M(35));
}

/* ------------------------------------------------------------ コマンドループ */

static int command_loop(struct sess *s) {
    int group = 0;
    bool shown = false;
    for (;;) {
        online_set_place(s->no, "COMMAND");
        pthread_mutex_lock(&g_lock);
        bool menu = USER(s)->menu, always = USER(s)->menu_always;
        pthread_mutex_unlock(&g_lock);
        if (menu && (always || !shown)) {
            CHK(show_menu(s, group));
            shown = true;
        }
        char line[256];
        CHK(term_readline(s->t, M(menu ? 120 : 141), line, sizeof line, 0));
        if (!line[0]) continue;
        if (menu && isdigit((unsigned char)line[0])) {
            int k = atoi(line);
            if (group == 0) {
                const struct command *items[16];
                if (k == 9) {
                    int r = run_command(s, by_name("OFF"), "");
                    if (r) return r < 0 ? r : 0;
                } else if (k >= 1 && k <= 8 && group_items(s, k, items)) {
                    group = k;
                    shown = false;
                } else CHK(outm_nl(s, 142));
                continue;
            }
            if (k == 0) {
                group = 0;
                shown = false;
                continue;
            }
            const struct command *items[16];
            int n = group_items(s, group, items);
            if (k < 1 || k > n) {
                CHK(outm_nl(s, 142));
                continue;
            }
            int r = run_command(s, items[k - 1], "");
            if (r) return r < 0 ? r : 0;
            continue;
        }
        char cmd[32];
        const char *arg = "";
        size_t k = strcspn(line, " \t");
        snprintf(cmd, sizeof cmd, "%.*s", (int)k, line);
        str_upper(cmd);
        if (line[k]) arg = line + k + strspn(line + k, " \t");
        if (cmd[0] == '?') {
            if (menu) CHK(show_menu(s, group));
            else CHK(help(s));
            continue;
        }
        /* \INDEX でそのボードを読む (メニュー方式では使えない) */
        if (cmd[0] == '\\' && !menu) {
            pthread_mutex_lock(&g_lock);
            struct board *b = board_by_index(cmd + 1);
            int no = b && board_can_read(USER(s), b) ? b->no : -1;
            pthread_mutex_unlock(&g_lock);
            if (no < 0) CHK(outm_nl(s, 335));
            else CHK(read_board(s, no, RB_NORMAL));
            continue;
        }
        const struct command *c = find_command(s, cmd);
        if (!c) {
            CHK(outm_nl(s, 142));
            continue;
        }
        int r = run_command(s, c, arg);
        if (r) return r < 0 ? r : 0;
    }
}

/* ------------------------------------------------------------ 1 回線ぶん */

static void account_logout(struct sess *s, time_t now) {
    struct user *u = USER(s);
    long used = (long)(now - s->login_at);
    pthread_mutex_lock(&g_lock);
    u->total_sec += used;
    u->month_sec += used;
    u->today_sec += used;
    /* 本日の残りは、数えた時間だけ減らす */
    int counted = s->start_left - g_online[s->no].left;
    int left = u->today_left - counted / 60;
    if (!g_online[s->no].unlimited) u->today_left = left < 0 ? 0 : left;
    if (!u->secret) {
        struct logent e = {s->login_at, now, s->no, u->id, "", ""};
        snprintf(e.logname, sizeof e.logname, "%s", u->logname);
        snprintf(e.speed, sizeof e.speed, "%s", s->speed);
        log_add(&e);
    }
    db_save_users();
    pthread_mutex_unlock(&g_lock);
}

void *session_thread(void *arg) {
    struct conn_arg *ca = arg;
    session_run(ca);
    close(ca->fd);
    online_free(ca->no);
    free(ca);
    return NULL;
}

void session_run(struct conn_arg *ca) {
    struct term t;
    struct sess s = {.t = &t, .no = ca->no, .uid = -1};
    snprintf(s.peer, sizeof s.peer, "%s", ca->peer);
    snprintf(s.speed, sizeof s.speed, "%s", ca->speed[0] ? ca->speed : "TCP");
    term_init(&t, ca->fd, ca->no, g_online[ca->no].notify_rd, ca->code[0] ? ca->code : g_cfg.default_code,
              !ca->serial);
    t.carrier = ca->carrier;
    nc_log("CH%02d: 接続 %s", ca->no, ca->peer);

    int r = login(&s);
    if (r == 0) {
        struct user *u = USER(&s);
        time_t now = time(NULL);
        s.login_at = now;
        pthread_mutex_lock(&g_lock);
        /* 日付が変わっていたら持ち時間を戻す */
        struct tm a, b;
        localtime_r(&now, &a);
        time_t last = u->last_login;
        localtime_r(&last, &b);
        if (!last || a.tm_yday != b.tm_yday || a.tm_year != b.tm_year) {
            u->today_left = u->day_minutes;
            u->today_sec = 0;
        }
        if (last && (a.tm_mon != b.tm_mon || a.tm_year != b.tm_year)) {
            u->prev_month_sec = u->month_sec;
            u->month_sec = 0;
            u->logins_prev = u->logins_month;
            u->logins_month = 0;
        }
        u->prev_login = u->last_login;
        u->last_login = now;
        u->logins++;
        u->logins_month++;
        g_sys.total_logins++;
        if (s.uid == 0) g_sys.guest_logins++;
        db_save_users();
        db_save_sys();
        char idbuf[16];
        snprintf(idbuf, sizeof idbuf, "%d", u->id);
        bool chat_on = u->chat_on_login, secret = u->secret;
        msg_set_esc(u->esc);
        t.bs_one_col = u->bs_one_col;
        /* 持ち時間 (ゲストと、1 日分を使い切った会員は最低持ち時間) */
        int minutes = s.uid == 0 ? g_sys.min_minutes : u->today_left;
        if (minutes < g_sys.min_minutes) minutes = g_sys.min_minutes;
        bool unlimited = u->level >= LV_SYSOP;
        pthread_mutex_unlock(&g_lock);
        online_set_user(s.no, idbuf, u->logname);
        g_online[s.no].chat = chat_on;
        g_online[s.no].secret = secret;
        pthread_mutex_lock(&g_lock);
        g_online[s.no].uid = u->id;
        g_online[s.no].left = minutes * 60;
        g_online[s.no].unlimited = unlimited;
        pthread_mutex_unlock(&g_lock);
        s.start_left = minutes * 60;
        nc_log("CH%02d: %04d %s がログイン", s.no, u->id, u->logname);

        if (u->flags & UF_NEW) r = user_setup(&s, true);
        if (r >= 0) r = opening(&s);
        if (r >= 0) {
            notify_others(&s, 160);
            login_call(&s);
            if (s.uid == 0 && g_sys.signup == SIGNUP_AUTO) r = signup_auto(&s, true);
            if (r >= 0) r = command_loop(&s);
        }
        if (r == T_TIMEUP) outm_nl(&s, 129);
        else if (r >= 0) {
            long used = (long)(time(NULL) - s.login_at);
            out(&s, "%s\n%02ld:%02ld:%02ld\n", g_sys.sysmes[4], used / 3600, used / 60 % 60, used % 60);
        }
        account_logout(&s, time(NULL));
        notify_others(&s, 161);
        nc_log("CH%02d: %04d %s がログアウト", s.no, u->id, u->logname);
    }
    nc_log("CH%02d: 切断 %s", ca->no, ca->peer);
    free(s.draft_title);
    free(s.draft_body);
    free(s.upload);
    term_free(&t);
}
