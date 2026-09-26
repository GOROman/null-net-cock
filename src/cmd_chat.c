/*
 * チャット: CHAT / LLIST / COFF / CON と、CMODE (チャットコール) / IMODE / LCSET (ログインコール)
 *
 * 回線 i から j に届くのは、j がチャットを受け付けていて (CON)、i の j に対する状態が on、
 * j の i に対する状態が off でない (on か 受信だけ) とき。
 */
#include "session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CS_ON = 0, CS_OFF = 1, CS_REC = 2 };

static const char *state_name(const struct online *o, int peer) {
    if (!o->chat) return "off";
    return o->chat_to[peer] == CS_OFF ? "off" : o->chat_to[peer] == CS_REC ? "rec" : "on";
}

/* 今この回線でチャットできるか (CTIME) */
static bool chat_allowed(int no) {
    pthread_mutex_lock(&g_lock);
    bool ok = !(g_sys.chat_limited[no] && time_forbidden());
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* ------------------------------------------------------------ LLIST */

int cmd_llist(struct sess *s, const char *arg) {
    CHK(outm_nl(s, 148));
    CHK(outm_nl(s, 149));
    for (int i = 1; i <= g_cfg.max_lines; i++) {
        char line[256];
        pthread_mutex_lock(&g_lock);
        struct online *o = &g_online[i], *me = &g_online[s->no];
        if (!o->used || o->uid < 0 || (o->secret && !IS_SYSOP(s))) {
            snprintf(line, sizeof line, "%2d%c\t%s\n", i, i == s->no ? '*' : '.',
                     g_sys.lines[i].level > 100 ? M(145) : M(144));
        } else {
            char left[16];
            if (o->unlimited) snprintf(left, sizeof left, "  -  ");
            else snprintf(left, sizeof left, "%5d%c", o->left, o->counting ? '*' : ' ');
            char flag[16];
            if (i == s->no) snprintf(flag, sizeof flag, "%s", o->chat ? "on" : "off");
            else snprintf(flag, sizeof flag, "%s/%s", state_name(o, s->no), state_name(me, i));
            snprintf(line, sizeof line, "%2d%c\t%-8s %-8s %s\t%04d:%s\n", i, i == s->no ? '*' : '.', o->place, flag,
                     left, o->uid, o->handle);
        }
        pthread_mutex_unlock(&g_lock);
        CHK(out(s, "%s", line));
    }
    return 0;
}

static int set_chat(struct sess *s, bool on) {
    pthread_mutex_lock(&g_lock);
    g_online[s->no].chat = on;
    memset(g_online[s->no].chat_to, on ? CS_ON : CS_OFF, sizeof g_online[s->no].chat_to);
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, on ? 151 : 150);
}

int cmd_coff(struct sess *s, const char *arg) { return set_chat(s, false); }
int cmd_con(struct sess *s, const char *arg) { return set_chat(s, true); }

/* ------------------------------------------------------------ 発言を届ける */

enum { TO_ALL = 0, TO_LOCAL = -1 };

/* to: TO_ALL / TO_LOCAL / 回線番号。届いた数を返す */
static int chat_send(struct sess *s, const char *text, int to) {
    struct {
        int no;
        char msg[512];
    } out_[MAX_LINES];
    int n = 0;
    pthread_mutex_lock(&g_lock);
    struct online *me = &g_online[s->no];
    const struct user *su = USER(s);
    for (int j = 1; j <= g_cfg.max_lines; j++) {
        struct online *o = &g_online[j];
        bool self = j == s->no;
        if (!o->used || o->uid < 0) continue;
        if (to > 0 && j != to && !self) continue;
        if (to == TO_LOCAL && !me->local[j] && !self) continue;
        const struct user *ru = &g_users[o->uid];
        if (self) {
            if (!su->chat_self) continue;
        } else {
            if (!o->chat || me->chat_to[j] != CS_ON || o->chat_to[s->no] == CS_OFF) continue;
            if (g_sys.chat_limited[j] && time_forbidden()) continue;
        }
        char name[80];
        if (!ru->chat_byid) snprintf(name, sizeof name, "%s", su->logname);
        else snprintf(name, sizeof name, "%04d", su->id);
        if (ru->chat_esc) snprintf(out_[n].msg, sizeof out_[n].msg, "\x1b[%dm(%s)\x1b[m%s", 31 + s->no % 7, name, text);
        else snprintf(out_[n].msg, sizeof out_[n].msg, "(%s)%s", name, text);
        out_[n].no = j;
        n++;
    }
    pthread_mutex_unlock(&g_lock);
    int delivered = 0;
    for (int k = 0; k < n; k++)
        if (notice_push(out_[k].no, N_CHAT, out_[k].msg) && out_[k].no != s->no) delivered++;
    return delivered;
}

/* 行の並び「125」「1,2,5」を回線の集合にする。空なら全部 */
static void parse_lines(const char *p, bool *set) {
    bool any = false;
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p)) continue;
        int v = 0;
        /* 区切りが無ければ 1 桁ずつ (原典の .s125)、区切りがあれば数として読む */
        if (strpbrk(p, ", ")) {
            v = (int)strtol(p, (char **)&p, 10);
            p--;
        } else v = *p - '0';
        if (v >= 1 && v <= MAX_LINES) set[v] = any = true;
    }
    if (!any)
        for (int i = 1; i <= MAX_LINES; i++) set[i] = true;
}

static int chat_help(struct sess *s) {
    return out(s, ".S[回線] 送受信 ON / .O[回線] OFF / .R[回線] 受信だけ / .C シスオペを呼ぶ / .T 右寄せ / .B 中央寄せ\n"
                  ".X 設定 / .Pn 回線 n だけに送る / .U回線 ローカル送出先 / .A ローカルだけに送る / .L 回線一覧 / "
                  ".Q 抜ける\n");
}

/* 80 桁基準で右寄せ (right) か中央寄せにする */
static void align(const char *text, char *buf, size_t sz, bool right) {
    int w = (int)utf8_width(text);
    int padn = w >= 70 ? 0 : right ? 70 - w : (70 - w) / 2;
    snprintf(buf, sz, "%*s%s", padn, "", text);
}

static int chat_settings(struct sess *s) {
    CHK(outm_nl(s, 222));
    char a[8];
    struct user u;
    pthread_mutex_lock(&g_lock);
    u = *USER(s);
    pthread_mutex_unlock(&g_lock);
    CHK(ask(s, 223, a, sizeof a, RL_UPPER));
    if (a[0]) u.chat_esc = a[0] == 'N';
    int r;
    if ((r = yn(s, 224)) < 0) return r;
    u.chat_noecho = !r;
    if ((r = yn(s, 225)) < 0) return r;
    u.chat_self = r;
    if ((r = yn(s, 226)) < 0) return r;
    u.chat_byid = !r;
    pthread_mutex_lock(&g_lock);
    USER(s)->chat_esc = u.chat_esc;
    USER(s)->chat_noecho = u.chat_noecho;
    USER(s)->chat_self = u.chat_self;
    USER(s)->chat_byid = u.chat_byid;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

static int call_sysop(struct sess *s) {
    pthread_mutex_lock(&g_lock);
    int need = g_sys.chat_call_level;
    bool ok = need < 255 && USER(s)->level >= need;
    char text[256];
    snprintf(text, sizeof text, "*** CH%02d %04d:%s がシスオペを呼んでいます ***", s->no, s->uid, USER(s)->logname);
    int ops[MAX_LINES], nops = 0;
    for (int i = 1; i <= g_cfg.max_lines; i++)
        if (i != s->no && g_online[i].used && g_online[i].uid >= 0 && g_users[g_online[i].uid].level >= LV_SYSOP)
            ops[nops++] = i;
    pthread_mutex_unlock(&g_lock);
    if (!ok) return outm_nl(s, 214);
    nc_log("CH%02d: チャットコール", s->no);
    for (int i = 0; i < nops; i++) notice_push(ops[i], N_SYSTEM, text);
    return outm_nl(s, 215);
}

/* ------------------------------------------------------------ CHAT */

int cmd_chat(struct sess *s, const char *arg) {
    if (!chat_allowed(s->no)) return outm_nl(s, 390);
    pthread_mutex_lock(&g_lock);
    bool was = g_online[s->no].chat;
    g_online[s->no].chat = true;
    pthread_mutex_unlock(&g_lock);
    CHK(outm_nl(s, 162));
    int r = 0;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        bool echo = !USER(s)->chat_noecho;
        pthread_mutex_unlock(&g_lock);
        char text[600];
        if ((r = term_readline(s->t, echo ? M(156) : "", text, sizeof text, echo ? RL_RAW : RL_RAW | RL_NOECHO)) < 0)
            break;
        str_trim(text);
        if (!text[0]) continue;
        /* 150 文字まで。制御コードは使えない */
        char clean[600];
        size_t w = 0;
        for (const char *p = text; *p && w < sizeof clean - 1; p++)
            if ((unsigned char)*p >= 0x20) clean[w++] = *p;
        clean[w] = 0;
        char buf[700];
        const char *t = clean;
        /* 全角の別の書き方を . の形にそろえる */
        if (!strncmp(t, "＊", 3) && !t[3]) t = ".L";
        else if (!strncmp(t, "＊", 3)) snprintf(buf, sizeof buf, ".T%s", t + 3), t = buf;
        else if (!strncmp(t, "／", 3)) snprintf(buf, sizeof buf, ".B%s", t + 3), t = buf;
        else if (!strncmp(t, "＝", 3)) snprintf(buf, sizeof buf, ".A%s", t + 3), t = buf;
        else if (!strncmp(t, "－", 3)) snprintf(buf, sizeof buf, ".P%s", t + 3), t = buf;
        else if (!strcmp(t, "*")) t = ".L";
        if (!strcmp(t, "Q") || !strcmp(t, "q") || !strcmp(t, ".") || !strcasecmp(t, ".Q")) {
            r = outm_nl(s, 157);
            break;
        }
        if (!strcmp(t, "?")) {
            if ((r = chat_help(s)) < 0) break;
            continue;
        }
        if (t[0] != '.') {
            if (!chat_allowed(s->no)) {
                if ((r = outm_nl(s, 390)) < 0) break;
                continue;
            }
            chat_send(s, t, TO_ALL);
            continue;
        }
        char c = (char)toupper((unsigned char)t[1]);
        const char *rest = t + 2;
        switch (c) {
        case 'S':
        case 'O':
        case 'R': {
            bool set[MAX_LINES + 1] = {0};
            parse_lines(rest, set);
            pthread_mutex_lock(&g_lock);
            for (int i = 1; i <= MAX_LINES; i++)
                if (set[i]) g_online[s->no].chat_to[i] = c == 'S' ? CS_ON : c == 'O' ? CS_OFF : CS_REC;
            pthread_mutex_unlock(&g_lock);
            r = outm_nl(s, 220);
            break;
        }
        case 'C':
            r = call_sysop(s);
            break;
        case 'T':
        case 'B': {
            char al[700];
            align(rest, al, sizeof al, c == 'T');
            chat_send(s, al, TO_ALL);
            break;
        }
        case 'X':
            r = chat_settings(s);
            break;
        case 'P': {
            /* 半角・全角の数字どちらも */
            char num[8] = "";
            const char *p = rest;
            size_t k = 0;
            while (k < sizeof num - 1) {
                if (isdigit((unsigned char)*p)) num[k++] = *p++;
                else if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC && (unsigned char)p[2] >= 0x90 &&
                         (unsigned char)p[2] <= 0x99)
                    num[k++] = (char)('0' + p[2] - 0x90), p += 3;
                else break;
            }
            num[k] = 0;
            while (*p == ' ') p++;
            int to = atoi(num);
            if (to < 1 || to > MAX_LINES || !chat_send(s, p, to)) r = outm_nl(s, 144);
            break;
        }
        case 'U': {
            bool set[MAX_LINES + 1] = {0};
            parse_lines(rest, set);
            pthread_mutex_lock(&g_lock);
            memcpy(g_online[s->no].local, set, sizeof set);
            pthread_mutex_unlock(&g_lock);
            r = outm_nl(s, 220);
            break;
        }
        case 'A':
            chat_send(s, rest, TO_LOCAL);
            break;
        case 'L':
            r = cmd_llist(s, "");
            if (r >= 0) {
                /* ローカル送出先に = */
                char buf2[256] = "";
                pthread_mutex_lock(&g_lock);
                for (int i = 1; i <= g_cfg.max_lines; i++)
                    if (g_online[s->no].local[i]) {
                        size_t l = strlen(buf2);
                        snprintf(buf2 + l, sizeof buf2 - l, " =%d", i);
                    }
                pthread_mutex_unlock(&g_lock);
                if (buf2[0]) r = out(s, "%s\n", buf2);
            }
            break;
        default:
            r = chat_help(s);
        }
        if (r < 0) break;
    }
    pthread_mutex_lock(&g_lock);
    g_online[s->no].chat = was || g_online[s->no].chat;
    pthread_mutex_unlock(&g_lock);
    return r < 0 ? r : 0;
}

/* ------------------------------------------------------------ CMODE / IMODE / LCSET */

int cmd_cmode(struct sess *s, const char *arg) {
    char a[8];
    CHK(ask(s, 377, a, sizeof a, RL_UPPER));
    static const char keys[] = "NLMHSO";
    static const int levels[] = {0, 30, 40, 80, 100, 255};
    const char *p = a[0] ? strchr(keys, a[0]) : NULL;
    pthread_mutex_lock(&g_lock);
    if (p) g_sys.chat_call_level = levels[p - keys];
    int lv = g_sys.chat_call_level;
    db_save_sys();
    pthread_mutex_unlock(&g_lock);
    if (lv == 255) return out(s, "%sOFF\n", M(378));
    return out(s, "%s%d\n", M(378), lv);
}

int cmd_imode(struct sess *s, const char *arg) {
    int menu = yn(s, 138);
    if (menu < 0) return menu;
    int always = 0;
    if (menu && (always = yn(s, 139)) < 0) return always;
    CHK(outm_nl(s, 222));
    char a[8];
    CHK(ask(s, 223, a, sizeof a, RL_UPPER));
    int echo = yn(s, 224);
    if (echo < 0) return echo;
    int self = yn(s, 225);
    if (self < 0) return self;
    int name = yn(s, 226);
    if (name < 0) return name;
    pthread_mutex_lock(&g_lock);
    g_sys.imode_menu = menu;
    g_sys.imode_menu_always = always;
    g_sys.imode_chat_esc = a[0] == 'N';
    g_sys.imode_chat_noecho = !echo;
    g_sys.imode_chat_self = self;
    g_sys.imode_chat_byid = !name;
    db_save_sys();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

int cmd_lcset(struct sess *s, const char *arg) {
    struct user *u = choose_user(s, 216);
    if (!u) return 0;
    char a[8];
    CHK(ask(s, 382, a, sizeof a, RL_UPPER));
    int v = a[0] == 'M' ? 1 : a[0] == 'B' ? 2 : a[0] == 'C' ? 0 : -1;
    if (v < 0) return 0;
    pthread_mutex_lock(&g_lock);
    u->lcall = v;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

/* ログインコールが設定された会員がログインしたら、ログに残し、ログイン中の SYSOP に知らせる */
void login_call(struct sess *s) {
    pthread_mutex_lock(&g_lock);
    bool call = USER(s)->lcall != 0;
    char text[256];
    snprintf(text, sizeof text, "*** ログインコール: CH%02d %04d:%s ***", s->no, s->uid, USER(s)->logname);
    int ops[MAX_LINES], nops = 0;
    if (call)
        for (int i = 1; i <= g_cfg.max_lines; i++)
            if (i != s->no && g_online[i].used && g_online[i].uid >= 0 && g_users[g_online[i].uid].level >= LV_SYSOP)
                ops[nops++] = i;
    pthread_mutex_unlock(&g_lock);
    if (!call) return;
    nc_log("%s", text);
    for (int i = 0; i < nops; i++) notice_push(ops[i], N_SYSTEM, text);
}
