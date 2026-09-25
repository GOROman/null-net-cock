/*
 * ボードとメール: BREAD / BWRITE / RALL / RNALL / BSET / PMOVE / MREAD / MWRITE / MCHECK / MBSET
 */
#include "session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ 一覧 */

void print_board_list(struct sess *s, bool for_write) {
    int count = 0;
    long total = 0, keep = 0;
    for (int i = 1; i < MAX_BOARDS; i++) {
        pthread_mutex_lock(&g_lock);
        struct board *b = &g_boards[i];
        bool show = b->used && (for_write ? board_can_write(USER(s), b) : board_can_read(USER(s), b));
        char line[512] = "";
        if (show) {
            int n = board_count(i), un = board_unread(s->uid, i);
            bool w = board_can_write(USER(s), b);
            const struct user *op = b->bop ? user_get(b->bop) : NULL;
            snprintf(line, sizeof line, "%-10s %s %4d/%-4d %s%s%s%s %s\n", b->index, w ? M(376) : M(375), un, n,
                     b->title, op ? " " : "", op ? M(373) : "", op ? op->logname : "", op ? M(374) : "");
            total += n;
            keep += b->keep;
        }
        pthread_mutex_unlock(&g_lock);
        if (!show) continue;
        if (out(s, "%s", line) < 0 || paging(s, &count, false)) return;
    }
    out(s, "%s%ld%s%ld\n", M(230), total, M(231), keep);
}

/* ------------------------------------------------------------ 1 通表示 */

static const char *name_of(int id) {
    const struct user *u = user_get(id);
    return u ? u->logname : "(削除)";
}

/* 表示したら 0、無ければ 1 */
static int show_msg(struct sess *s, int board, int seq) {
    pthread_mutex_lock(&g_lock);
    struct msg *m = msg_get(board, seq);
    if (!m || (board == MAIL_BOARD && !mail_is_for(m, s->uid, NULL) && m->from != s->uid && !IS_SYSOP(s))) {
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    m->reads++;
    struct msg c = *m;
    char *body = msg_body(m);
    int total = board == MAIL_BOARD ? 0 : g_boards[board].next_seq - 1;
    char idx[16], from[64], to[256] = "";
    snprintf(idx, sizeof idx, "%s", board == MAIL_BOARD ? "MAIL" : g_boards[board].index);
    snprintf(from, sizeof from, "%s", name_of(c.from));
    if (board == MAIL_BOARD)
        for (int k = 0; k < MAX_MAIL_TO; k++)
            if (c.to[k]) {
                size_t l = strlen(to);
                snprintf(to + l, sizeof to - l, "%s%04d:%s", l ? ", " : "", c.to[k], name_of(c.to[k]));
            }
    if (board != MAIL_BOARD && *read_ptr(s->uid, board) < seq) *read_ptr(s->uid, board) = seq;
    pthread_mutex_unlock(&g_lock);

    char ts[32];
    fmt_time(c.t, ts, sizeof ts);
    int r = 0;
    if (total) r = out(s, "\n%s(%d/%d) %s %s\n", idx, c.seq, total, ts, c.title);
    else r = out(s, "\n%s(%d) %s %s\n", idx, c.seq, ts, c.title);
    if (r >= 0) r = out(s, "%ld bytes  %04d:%s", c.len, c.from, from);
    if (r >= 0 && c.reply_to) r = out(s, "  (%d へのリプライ)", c.reply_to);
    if (r >= 0 && c.replies) r = out(s, "  リプライ %d 件", c.replies);
    if (r >= 0 && to[0]) r = out(s, "\n宛先: %s", to);
    if (r >= 0) r = out(s, "\n\n%s%s", body, body[0] && body[strlen(body) - 1] != '\n' ? "\n" : "");
    free(body);
    if (r < 0) return r;
    s->last_board = board;
    s->last_seq = seq;
    return 0;
}

/* board の中で cur の次 (dir=1) / 前 (dir=-1) のメッセージ番号。無ければ 0 */
static int neighbor(struct sess *s, int board, int cur, int dir) {
    int best = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmsgs; i++) {
        struct msg *m = &g_msgs[i];
        if (m->board != board || m->deleted) continue;
        if (board == MAIL_BOARD && !mail_is_for(m, s->uid, NULL)) continue;
        if (dir > 0 && m->seq > cur && (!best || m->seq < best)) best = m->seq;
        if (dir < 0 && m->seq < cur && m->seq > best) best = m->seq;
    }
    pthread_mutex_unlock(&g_lock);
    return best;
}

static int list_titles(struct sess *s, int board, int from) {
    int count = 0;
    for (int seq = neighbor(s, board, from - 1, 1); seq; seq = neighbor(s, board, seq, 1)) {
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(board, seq);
        char line[512] = "";
        if (m) {
            char ts[32];
            fmt_time(m->t, ts, sizeof ts);
            snprintf(line, sizeof line, "%4d %s %04d:%-12s %s%s\n", m->seq, ts, m->from, name_of(m->from),
                     m->reply_to ? "Re " : "", m->title);
        }
        pthread_mutex_unlock(&g_lock);
        CHK(out(s, "%s", line));
        int r = paging(s, &count, false);
        if (r) return r < 0 ? r : 0;
    }
    return 0;
}

/* ------------------------------------------------------------ 書き込み */

/* 「.」だけの行まで本文を読む。*body に足していく */
static int edit_body(struct sess *s, char **body) {
    size_t len = *body ? strlen(*body) : 0;
    for (;;) {
        char line[LINE_MAX_BYTES];
        CHK(term_readline(s->t, "", line, sizeof line, RL_RAW));
        if (!strcmp(line, ".")) return 0;
        size_t l = strlen(line);
        if (len + l + 2 > MAX_BODY_BYTES) {
            CHK(out(s, "\a"));
            return 0;
        }
        *body = realloc(*body, len + l + 2);
        memcpy(*body + len, line, l);
        len += l;
        (*body)[len++] = '\n';
        (*body)[len] = 0;
    }
}

static int ask_title(struct sess *s, char *title, size_t sz, const char *def) {
    if (def && def[0]) CHK(out(s, "(Enter: %s)\n", def));
    CHK(ask(s, 191, title, sz, 0));
    if (!title[0] && def) snprintf(title, sz, "%s", def);
    /* 全角 50 文字 (表示幅 100) まで */
    while (utf8_width(title) > 100) {
        size_t n = strlen(title);
        do n--; while (n > 0 && ((unsigned char)title[n] & 0xC0) == 0x80);
        title[n] = 0;
    }
    return 0;
}

/* 書いて保存する。書いたら 1、やめたら 0 */
static int write_msg(struct sess *s, int board, int reply_to, const int *to) {
    char title[MAX_TITLE_BYTES + 1] = "", def[MAX_TITLE_BYTES + 1] = "";
    char *body = NULL;
    int r;
    /* 書きかけ (Q で抜けたもの) があれば続きから */
    if (s->draft_body && s->draft_board == board && !reply_to && !to) {
        r = term_yesno(s->t, "書きかけの文章があります。続きを書きますか");
        if (r < 0) return r;
        if (r) {
            snprintf(title, sizeof title, "%s", s->draft_title ? s->draft_title : "");
            body = s->draft_body;
            s->draft_body = NULL;
            free(s->draft_title);
            s->draft_title = NULL;
        }
    }
    if (!body) {
        if (reply_to) {
            pthread_mutex_lock(&g_lock);
            struct msg *p = msg_get(board, reply_to);
            if (p) snprintf(def, sizeof def, "%s%.190s", strncmp(p->title, "Re:", 3) ? "Re:" : "", p->title);
            pthread_mutex_unlock(&g_lock);
        }
        CHK(ask_title(s, title, sizeof title, def));
        if (!title[0]) return outm_nl(s, 185);
        CHK(outm_nl(s, 192));
        CHK(outm_nl(s, 193));
        if ((r = edit_body(s, &body)) < 0) goto fail;
    }
    for (;;) {
        char a[16];
        if ((r = ask(s, 194, a, sizeof a, RL_UPPER)) < 0) goto fail;
        switch (a[0]) {
        case 'Y': {
            struct msg m = {.board = board, .from = s->uid, .reply_to = reply_to};
            snprintf(m.title, sizeof m.title, "%s", title);
            if (to)
                for (int k = 0; k < MAX_MAIL_TO; k++) {
                    m.to[k] = to[k];
                    m.to_state[k] = to[k] ? 'n' : 0;
                }
            pthread_mutex_lock(&g_lock);
            int seq = msg_add(&m, body ? body : "");
            if (board == MAIL_BOARD) USER(s)->wm++;
            else USER(s)->wb++;
            if (board != MAIL_BOARD && *read_ptr(s->uid, board) == seq - 1) *read_ptr(s->uid, board) = seq;
            db_save_users();
            pthread_mutex_unlock(&g_lock);
            free(body);
            nc_log("CH%02d: %04d がボード %d に %d 番を書き込み", s->no, s->uid, board, seq);
            CHK(outm_nl(s, 195));
            return 1;
        }
        case 'N':
            free(body);
            CHK(outm_nl(s, 188));
            return 0;
        case 'T':
            if ((r = ask_title(s, title, sizeof title, title)) < 0) goto fail;
            break;
        case 'C':
            if ((r = edit_body(s, &body)) < 0) goto fail;
            break;
        case 'L':
            if ((r = out(s, "%s\n%s", title, body ? body : "")) < 0) goto fail;
            break;
        case 'Q':
            free(s->draft_title);
            free(s->draft_body);
            s->draft_title = strdup(title);
            s->draft_body = body;
            s->draft_board = board;
            CHK(outm_nl(s, 189));
            return 0;
        default:
            if ((r = out(s, "Y:書き込む N:捨てる T:タイトルを変える C:続きを書く L:表示 Q:書きかけで抜ける\n")) < 0)
                goto fail;
        }
    }
fail:
    free(body);
    return r;
}

/* ------------------------------------------------------------ ボードを読む */

static int delete_msg(struct sess *s, int board, int seq) {
    pthread_mutex_lock(&g_lock);
    struct msg *m = msg_get(board, seq);
    bool ok = m && (m->from == s->uid || IS_SYSOP(s) || (board != MAIL_BOARD && g_boards[board].bop == s->uid));
    pthread_mutex_unlock(&g_lock);
    if (!ok) return out(s, "==== 削除できません ====\n");
    int r = yn(s, 221);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    m = msg_get(board, seq);
    if (m) {
        m->deleted = true;
        db_save_msgs();
    }
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 204);
}

static int read_help(struct sess *s) {
    return out(s, "Enter/N:次 B:前 番号:その番号 R:もう一度 A:続けて全部 L:タイトル一覧 T:先頭からの一覧\n"
                  "W:書く F:リプライ D:削除 P:既読位置をここに E:最後へ Q:終わる\n");
}

/* mode 0: 対話で読む  mode 1: 未読を止まらずに全部表示 */
int read_board(struct sess *s, int board, int mode) {
    pthread_mutex_lock(&g_lock);
    struct board *b = &g_boards[board];
    int n = board_count(board), un = board_unread(s->uid, board), cur = *read_ptr(s->uid, board);
    char head[256];
    snprintf(head, sizeof head, "\n[%s] %s  %d 件 (未読 %d)\n", b->index, b->title, n, un);
    pthread_mutex_unlock(&g_lock);
    online_set_place(s->no, b->index);
    CHK(out(s, "%s", head));
    if (n == 0) return outm_nl(s, 197);
    if (mode == 1) {
        for (int seq; (seq = neighbor(s, board, cur, 1)); cur = seq) CHK(show_msg(s, board, seq));
        return 0;
    }
    for (;;) {
        char a[32];
        CHK(ask(s, 196, a, sizeof a, RL_UPPER));
        int num = isdigit((unsigned char)a[0]) ? atoi(a) : isdigit((unsigned char)a[1]) ? atoi(a + 1) : 0;
        char c = isdigit((unsigned char)a[0]) ? '#' : a[0];
        int seq;
        switch (c) {
        case 0:
        case 'N':
            seq = neighbor(s, board, cur, 1);
            if (!seq) {
                CHK(outm_nl(s, 198));
                return 0;
            }
            CHK(show_msg(s, board, seq));
            cur = seq;
            break;
        case 'B':
            seq = neighbor(s, board, cur, -1);
            if (!seq) {
                CHK(outm_nl(s, 197));
                break;
            }
            CHK(show_msg(s, board, seq));
            cur = seq;
            break;
        case '#':
        case 'R':
            seq = num ? num : cur;
            if (show_msg(s, board, seq)) CHK(outm_nl(s, 197));
            else cur = seq;
            break;
        case 'A':
            for (; (seq = neighbor(s, board, cur, 1)); cur = seq) CHK(show_msg(s, board, seq));
            CHK(outm_nl(s, 198));
            break;
        case 'L':
            CHK(list_titles(s, board, cur + 1));
            break;
        case 'T':
            CHK(list_titles(s, board, num ? num : 1));
            break;
        case 'W': {
            pthread_mutex_lock(&g_lock);
            bool w = board_can_write(USER(s), &g_boards[board]);
            pthread_mutex_unlock(&g_lock);
            if (!w) CHK(outm_nl(s, 183));
            else CHK(write_msg(s, board, 0, NULL));
            break;
        }
        case 'F': {
            pthread_mutex_lock(&g_lock);
            bool w = board_can_write(USER(s), &g_boards[board]);
            pthread_mutex_unlock(&g_lock);
            if (!w) CHK(outm_nl(s, 183));
            else if (!cur && !num) CHK(outm_nl(s, 197));
            else CHK(write_msg(s, board, num ? num : cur, NULL));
            break;
        }
        case 'D':
            CHK(delete_msg(s, board, num ? num : cur));
            break;
        case 'P':
            pthread_mutex_lock(&g_lock);
            *read_ptr(s->uid, board) = num ? num : cur;
            db_save_ptrs();
            pthread_mutex_unlock(&g_lock);
            break;
        case 'E':
            pthread_mutex_lock(&g_lock);
            cur = g_boards[board].next_seq - 1;
            *read_ptr(s->uid, board) = cur;
            pthread_mutex_unlock(&g_lock);
            break;
        case 'Q':
            pthread_mutex_lock(&g_lock);
            db_save_ptrs();
            pthread_mutex_unlock(&g_lock);
            return 0;
        default:
            CHK(read_help(s));
        }
    }
}

int cmd_bread(struct sess *s, const char *arg) {
    int b;
    if (arg[0]) {
        pthread_mutex_lock(&g_lock);
        struct board *bp = board_by_index(arg[0] == '\\' ? arg + 1 : arg);
        b = bp && board_can_read(USER(s), bp) ? bp->no : -1;
        pthread_mutex_unlock(&g_lock);
        if (b < 0) return outm_nl(s, 335);
    } else {
        b = choose_board(s, 181, false);
        if (b < -1) return b + 1;
        if (b < 0) return 0;
    }
    return read_board(s, b, 0);
}

int cmd_bwrite(struct sess *s, const char *arg) {
    int b;
    if (arg[0]) {
        pthread_mutex_lock(&g_lock);
        struct board *bp = board_by_index(arg[0] == '\\' ? arg + 1 : arg);
        b = bp ? bp->no : -1;
        bool w = bp && board_can_write(USER(s), bp);
        pthread_mutex_unlock(&g_lock);
        if (b < 0) return outm_nl(s, 335);
        if (!w) return outm_nl(s, 183);
    } else {
        b = choose_board(s, 182, true);
        if (b < -1) return b + 1;
        if (b < 0) return 0;
    }
    int r = write_msg(s, b, 0, NULL);
    return r < 0 ? r : 0;
}

/* 巡回: BSET で Y にした読めるボードを順に */
static int read_all(struct sess *s, int mode) {
    for (int i = 1; i < MAX_BOARDS; i++) {
        pthread_mutex_lock(&g_lock);
        bool target = g_boards[i].used && board_can_read(USER(s), &g_boards[i]) && *bset_flag(s->uid, i) == 'Y' &&
                      board_unread(s->uid, i) > 0;
        pthread_mutex_unlock(&g_lock);
        if (target) CHK(read_board(s, i, mode));
    }
    pthread_mutex_lock(&g_lock);
    db_save_ptrs();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 199);
}

int cmd_rall(struct sess *s, const char *arg) { return read_all(s, 0); }
int cmd_rnall(struct sess *s, const char *arg) { return read_all(s, 1); }

int cmd_bset(struct sess *s, const char *arg) {
    for (;;) {
        int b = choose_board(s, 180, false);
        if (b < -1) return b + 1;
        if (b < 0) return 0;
        int r = yn(s, 200);
        if (r < 0) return r;
        pthread_mutex_lock(&g_lock);
        *bset_flag(s->uid, b) = r ? 'Y' : 'N';
        db_save_ptrs();
        pthread_mutex_unlock(&g_lock);
        CHK(outm_nl(s, 220));
    }
}

/* 既読位置をまとめて動かす */
int cmd_pmove(struct sess *s, const char *arg) {
    char a[16];
    CHK(ask(s, 287, a, sizeof a, RL_UPPER));
    time_t when = 0;
    const char *label;
    switch (a[0]) {
    case 'T': label = M(288); when = 0; break;
    case 'E': label = M(289); when = (time_t)-1; break;
    case 'L':
        label = "前回のログイン";
        pthread_mutex_lock(&g_lock);
        when = USER(s)->prev_login;
        pthread_mutex_unlock(&g_lock);
        break;
    case 'D': {
        char d[32], t[32];
        CHK(ask(s, 290, d, sizeof d, 0));
        CHK(ask(s, 291, t, sizeof t, 0));
        struct tm tm = {0};
        int y, mo, dd, h = 0, mi = 0;
        if (sscanf(d, "%d/%d/%d", &y, &mo, &dd) != 3) return 0;
        sscanf(t, "%d:%d", &h, &mi);
        tm.tm_year = (y < 100 ? (y < 70 ? y + 100 : y) : y - 1900);
        tm.tm_mon = mo - 1;
        tm.tm_mday = dd;
        tm.tm_hour = h;
        tm.tm_min = mi;
        tm.tm_isdst = -1;
        when = mktime(&tm);
        label = d;
        break;
    }
    default:
        return 0;
    }
    CHK(out(s, "%s", label));
    int r = yn(s, 286);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    for (int b = 1; b < MAX_BOARDS; b++) {
        if (!g_boards[b].used) continue;
        int p = 0;
        for (int i = 0; i < g_nmsgs; i++) {
            struct msg *m = &g_msgs[i];
            if (m->board == b && (when == (time_t)-1 || m->t < when) && m->seq > p) p = m->seq;
        }
        if (when == (time_t)-1) p = g_boards[b].next_seq - 1;
        *read_ptr(s->uid, b) = p;
    }
    db_save_ptrs();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 285);
}

/* ------------------------------------------------------------ メール */

static int count_mail(int uid, bool unread_only) {
    int n = 0, slot;
    for (int i = 0; i < g_nmsgs; i++) {
        struct msg *m = &g_msgs[i];
        if (m->board == MAIL_BOARD && !m->deleted && mail_is_for(m, uid, &slot) &&
            (!unread_only || m->to_state[slot] == 'n'))
            n++;
    }
    return n;
}

int login_mail_notice(struct sess *s) {
    pthread_mutex_lock(&g_lock);
    int n = count_mail(s->uid, true);
    pthread_mutex_unlock(&g_lock);
    if (!n) return 0;
    return out(s, "%sMAIL%s%d%s\n", M(239), M(240), n, M(241));
}

/* 自分の分の状態 (n/r/d) を変える */
static void set_mail_state(int uid, int seq, char st) {
    struct msg *m = msg_get(MAIL_BOARD, seq);
    int slot;
    if (!m || !mail_is_for(m, uid, &slot)) return;
    m->to_state[slot] = st;
    /* 宛先全員が削除したら本体も削除 */
    bool all = true;
    for (int k = 0; k < MAX_MAIL_TO; k++)
        if (m->to[k] && m->to_state[k] != 'd') all = false;
    if (all) m->deleted = true;
    db_save_msgs();
}

/* 宛先を聞いてメールを書く。reply_from があればその人に返信 */
static int mail_to(struct sess *s, int reply_from) {
    int to[MAX_MAIL_TO] = {0};
    int n = 0;
    if (reply_from) {
        pthread_mutex_lock(&g_lock);
        const char *nm = name_of(reply_from);
        char buf[80];
        snprintf(buf, sizeof buf, "%s", nm);
        pthread_mutex_unlock(&g_lock);
        CHK(out(s, "%04d:%s%s\n", reply_from, buf, M(243)));
        to[n++] = reply_from;
    }
    while (n < MAX_MAIL_TO && !reply_from) {
        char prompt[64];
        snprintf(prompt, sizeof prompt, "%s%d%s", M(232), n + 1, M(233));
        char a[64];
        CHK(ask_str(s, prompt, a, sizeof a, 0));
        if (!a[0]) break;
        pthread_mutex_lock(&g_lock);
        struct user *u = user_find(a);
        int id = u ? u->id : -1, err = 0;
        char nm[64] = "";
        if (!u || id == 0) err = 135;
        else if (id == s->uid) err = 237;
        else if (u->mailbox_closed) err = 234;
        else
            for (int k = 0; k < n; k++)
                if (to[k] == id) err = 236;
        if (u) snprintf(nm, sizeof nm, "%s", u->logname);
        pthread_mutex_unlock(&g_lock);
        if (err) {
            CHK(outm_nl(s, err));
            continue;
        }
        CHK(out(s, "%04d:%s", id, nm));
        int r = yn(s, 235);
        if (r < 0) return r;
        if (r) to[n++] = id;
    }
    if (!n) return outm_nl(s, 186);
    int r = write_msg(s, MAIL_BOARD, 0, to);
    return r < 0 ? r : 0;
}

int cmd_mwrite(struct sess *s, const char *arg) { return mail_to(s, 0); }

/* 1 通読んで次の操作を聞く。1: やめる */
static int mail_step(struct sess *s, int seq) {
    CHK(show_msg(s, MAIL_BOARD, seq));
    pthread_mutex_lock(&g_lock);
    struct msg *m = msg_get(MAIL_BOARD, seq);
    int from = m ? m->from : 0;
    int slot;
    if (m && mail_is_for(m, s->uid, &slot) && m->to_state[slot] == 'n') set_mail_state(s->uid, seq, 'r');
    pthread_mutex_unlock(&g_lock);
    for (;;) {
        char a[16];
        CHK(ask(s, 365, a, sizeof a, RL_UPPER));
        switch (a[0]) {
        case 0:
        case 'Y':
            return 0;
        case 'D': {
            int r = yn(s, 366);
            if (r < 0) return r;
            if (r) {
                pthread_mutex_lock(&g_lock);
                set_mail_state(s->uid, seq, 'd');
                pthread_mutex_unlock(&g_lock);
                CHK(outm_nl(s, 367));
            }
            return 0;
        }
        case 'R':
            CHK(mail_to(s, from));
            return 0;
        case 'Q':
            return 1;
        }
    }
}

int cmd_mcheck(struct sess *s, const char *arg) {
    int cur = 0;
    for (;;) {
        int seq = 0;
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_nmsgs; i++) {
            struct msg *m = &g_msgs[i];
            int slot;
            if (m->board == MAIL_BOARD && !m->deleted && m->seq > cur && mail_is_for(m, s->uid, &slot) &&
                m->to_state[slot] == 'n' && (!seq || m->seq < seq))
                seq = m->seq;
        }
        pthread_mutex_unlock(&g_lock);
        if (!seq) break;
        int r = mail_step(s, seq);
        if (r < 0) return r;
        if (r) break;
        cur = seq;
    }
    return outm_nl(s, 361);
}

int cmd_mread(struct sess *s, const char *arg) {
    pthread_mutex_lock(&g_lock);
    int total = count_mail(s->uid, false);
    pthread_mutex_unlock(&g_lock);
    if (!total) return outm_nl(s, 197);
    /* 一覧 */
    int count = 0;
    for (int seq = neighbor(s, MAIL_BOARD, 0, 1); seq; seq = neighbor(s, MAIL_BOARD, seq, 1)) {
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(MAIL_BOARD, seq);
        char line[512] = "";
        int slot;
        if (m && mail_is_for(m, s->uid, &slot)) {
            char ts[32];
            fmt_time(m->t, ts, sizeof ts);
            snprintf(line, sizeof line, "%4d %s %s %04d:%-12s %s\n", m->seq,
                     M(m->to_state[slot] == 'n' ? 362 : m->to_state[slot] == 'r' ? 363 : 364), ts, m->from,
                     name_of(m->from), m->title);
        }
        pthread_mutex_unlock(&g_lock);
        CHK(out(s, "%s", line));
        int r = paging(s, &count, false);
        if (r < 0) return r;
        if (r) break;
    }
    for (;;) {
        char a[16];
        CHK(ask_str(s, "読むメールの番号 (Enter:終わる)>", a, sizeof a, 0));
        if (!a[0]) return 0;
        int seq = atoi(a);
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(MAIL_BOARD, seq);
        bool mine = m && mail_is_for(m, s->uid, NULL);
        pthread_mutex_unlock(&g_lock);
        if (!mine) {
            CHK(outm_nl(s, 197));
            continue;
        }
        int r = mail_step(s, seq);
        if (r < 0) return r;
        if (r) return 0;
    }
}

int cmd_mbset(struct sess *s, const char *arg) {
    int r = yn(s, 244);
    if (r < 0) return r;
    pthread_mutex_lock(&g_lock);
    USER(s)->mailbox_closed = r;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, r ? 245 : 246);
}
