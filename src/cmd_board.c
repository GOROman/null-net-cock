/*
 * ボードとメール: BREAD / BWRITE / RALL / RNALL / BSET / PMOVE / BATCH / MREAD / MWRITE / MCHECK / MBSET
 *
 * ボードを読むときは「今のメッセージ」(cur) のヘッダを出してサブコマンドを待つ。R (Enter) で本文を読み、
 * 次のメッセージへ進む。
 */
#include "session.h"
#include "xfer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *name_of(int id) {
    const struct user *u = user_get(id);
    return u ? u->logname : "(削除)";
}

/* ------------------------------------------------------------ 一覧 */

/* 「番号. \INDEX タイトル」。未読のあるボードには * */
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
            const struct user *op = b->bop ? user_get(b->bop) : NULL;
            char t[200];
            snprintf(line, sizeof line, "%c%2d. \\%-10s %s %4d/%-4d %s%04d:%s%s\n", un ? '*' : ' ', i, b->index,
                     pad(t, sizeof t, b->title, 40), un, n, M(373), b->bop, op ? op->logname : "-", M(374));
            total += n;
            keep += b->keep;
        }
        pthread_mutex_unlock(&g_lock);
        if (!show) continue;
        if (out(s, "%s", line) < 0 || paging(s, &count, false)) return;
    }
    out(s, "%s%ld%s%ld\n", M(230), total, M(231), keep);
}

/* ------------------------------------------------------------ メッセージを探す */

/* 自分が見られるメッセージか (メールは自分宛てで削除していないもの) */
static bool visible(struct sess *s, const struct msg *m, int board) {
    if (m->board != board || m->deleted) return false;
    if (board == MAIL_BOARD) return mail_is_for(m, s->uid, NULL);
    return true;
}

/* cur の次 (dir=1) / 前 (dir=-1) の番号。無ければ 0 */
static int neighbor(struct sess *s, int board, int cur, int dir) {
    int best = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmsgs; i++) {
        struct msg *m = &g_msgs[i];
        if (!visible(s, m, board)) continue;
        if (dir > 0 && m->seq > cur && (!best || m->seq < best)) best = m->seq;
        if (dir < 0 && m->seq < cur && m->seq > best) best = m->seq;
    }
    pthread_mutex_unlock(&g_lock);
    return best;
}

static bool exists(struct sess *s, int board, int seq) {
    pthread_mutex_lock(&g_lock);
    struct msg *m = msg_get(board, seq);
    bool ok = m && visible(s, m, board);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* ボードを開いたときの位置: 未読のうち最も古いもの (メールは未読 n のうち最も古いもの) */
static int first_unread(struct sess *s, int board) {
    int best = 0;
    pthread_mutex_lock(&g_lock);
    int ptr = board == MAIL_BOARD ? 0 : *read_ptr(s->uid, board);
    for (int i = 0; i < g_nmsgs; i++) {
        struct msg *m = &g_msgs[i];
        if (!visible(s, m, board) || m->seq <= ptr) continue;
        int slot;
        if (board == MAIL_BOARD && (!mail_is_for(m, s->uid, &slot) || m->to_state[slot] != 'n')) continue;
        if (!best || m->seq < best) best = m->seq;
    }
    pthread_mutex_unlock(&g_lock);
    return best;
}

/* ------------------------------------------------------------ 表示 */

static void fname_83(const char *name, char *out, size_t sz) {
    /* 「NAME    .EXT」の形に揃える */
    const char *dot = strrchr(name, '.');
    int bl = dot ? (int)(dot - name) : (int)strlen(name);
    snprintf(out, sz, "%-8.*s.%-3s", bl > 8 ? 8 : bl, name, dot ? dot + 1 : "");
}

/* ヘッダ (本文の前) を出す。最後の行は改行せずにサブコマンドのプロンプトへ続ける */
static int show_header(struct sess *s, int board, int seq) {
    pthread_mutex_lock(&g_lock);
    struct msg *m = msg_get(board, seq);
    if (!m) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    struct msg c = *m;
    struct board *b = &g_boards[board];
    char idx[16], from[64], re[256] = "", to[256] = "";
    snprintf(idx, sizeof idx, "%s", b->index);
    snprintf(from, sizeof from, "%s", name_of(c.from));
    int last = 0;
    for (int i = 0; i < g_nmsgs; i++)
        if (visible(s, &g_msgs[i], board) && g_msgs[i].seq > last) last = g_msgs[i].seq;
    if (c.reply_to) {
        struct msg *p = msg_get(board, c.reply_to);
        if (p) snprintf(re, sizeof re, "[re:%d %s/%04d:%s]\n", p->seq, p->title, p->from, name_of(p->from));
        else snprintf(re, sizeof re, "[re:%d]\n", c.reply_to);
    }
    if (board == MAIL_BOARD)
        for (int k = 0; k < MAX_MAIL_TO; k++)
            if (c.to[k]) {
                size_t l = strlen(to);
                snprintf(to + l, sizeof to - l, "%s%04d:%s", l ? " " : "  to ", c.to[k], name_of(c.to[k]));
            }
    bool prog = b->type == BT_PROGRAM && c.fid;
    pthread_mutex_unlock(&g_lock);

    char ts[32];
    fmt_time(c.t, ts, sizeof ts);
    if (board == MAIL_BOARD) CHK(out(s, "\nmail %s %s\n%s", ts, c.title, re));
    else CHK(out(s, "\n%s(%d/%d) %s %s\n%s", idx, c.seq, last, ts, c.title, re));
    if (prog) {
        char fn[16];
        fname_83(c.fname, fn, sizeof fn);
        CHK(out(s, "  [%s] %ldKB %04d:%s", fn, (c.fsize + 1023) / 1024, c.from, from));
    } else CHK(out(s, "  %ldbytes %04d:%s", c.len, c.from, from));
    if (c.replies) CHK(out(s, "  %dreply", c.replies));
    if (to[0]) CHK(out(s, "%s", to));
    return out(s, " ");
}

/* 本文を出して、既読にする */
static int show_body(struct sess *s, int board, int seq) {
    pthread_mutex_lock(&g_lock);
    struct msg *m = msg_get(board, seq);
    if (!m) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    m->reads++;
    char *body = msg_body(m);
    if (board == MAIL_BOARD) {
        int slot;
        if (mail_is_for(m, s->uid, &slot) && m->to_state[slot] == 'n') {
            m->to_state[slot] = 'r';
            db_save_msgs();
        }
    } else if (*read_ptr(s->uid, board) < seq) *read_ptr(s->uid, board) = seq;
    pthread_mutex_unlock(&g_lock);
    int r = out(s, "\n%s%s", body, body[0] && body[strlen(body) - 1] != '\n' ? "\n" : "");
    free(body);
    s->last_board = board;
    s->last_seq = seq;
    return r;
}

/* 20 件ぶんのタイトル。with_name なら発信者を名前で、でなければ ID で。最後に並べた番号を返す */
static int list_titles(struct sess *s, int board, int from, bool with_name, int *last) {
    *last = 0;
    for (int k = 0, seq = from; k < 20 && seq; k++, seq = neighbor(s, board, seq, 1)) {
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(board, seq);
        char line[512] = "";
        if (m) {
            char ts[32], who[80];
            fmt_time(m->t, ts, sizeof ts);
            if (with_name) pad(who, sizeof who, name_of(m->from), 14);
            else snprintf(who, sizeof who, "%04d", m->from);
            snprintf(line, sizeof line, "%5d %s %s %s%s\n", m->seq, ts, who, m->reply_to ? "re:" : "", m->title);
        }
        pthread_mutex_unlock(&g_lock);
        CHK(out(s, "%s", line));
        *last = seq;
    }
    return 0;
}

/* ------------------------------------------------------------ 書き込み */

/* 「.」だけの行まで本文を読む。*body に足していく */
static int edit_body(struct sess *s, char **body, bool allow_esc) {
    size_t len = *body ? strlen(*body) : 0;
    for (;;) {
        char line[LINE_MAX_BYTES];
        CHK(term_readline(s->t, "", line, sizeof line, RL_RAW));
        if (!strcmp(line, ".")) return 0;
        if (!allow_esc) {
            char *w = line;
            for (char *r = line; *r; r++) if (*r != 0x1B) *w++ = *r;
            *w = 0;
        }
        size_t l = strlen(line);
        if (len + l + 2 > MAX_BODY_BYTES) {
            CHK(out(s, "==== 大きさの上限 (%d バイト) を超えるので、ここで終わります ====\n", MAX_BODY_BYTES));
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

/* ファイルを受け取って s->upload に置く。受け取れたら 1 */
static int receive_file(struct sess *s) {
    if (s->upload) {
        int r = yn(s, 298);
        if (r) return r;
        free(s->upload);
        s->upload = NULL;
    }
    int proto, r;
    if ((r = yn(s, 299)) < 0) return r;
    if (r) {
        if ((r = yn(s, 301)) < 0) return r;
        proto = r ? XR_YMODEM_G : XR_YMODEM;
    } else {
        if ((r = yn(s, 300)) < 0) return r;
        proto = r ? XR_CRC : XR_SUM;
    }
    char name[64] = "";
    if (proto <= XR_CRC) {
        for (;;) {
            CHK(ask(s, 311, name, sizeof name, RL_UPPER));
            if (!name[0]) return 0;
            if ((r = yn(s, 312)) < 0) return r;
            if (r) break;
        }
    }
    CHK(outm_nl(s, proto == XR_SUM ? 302 : proto == XR_CRC ? 303 : proto == XR_YMODEM ? 304 : 305));
    online_set_place(s->no, "UPLOAD");
    unsigned char *data = NULL;
    size_t len = 0;
    r = xfer_recv(s->t, proto, name, sizeof name, &data, &len);
    if (r < 0) return r;
    if (r) return outm_nl(s, r == XF_CANCEL ? 309 : r == XF_BADNAME ? 310 : 308);
    if (len == 0) {
        free(data);
        return outm_nl(s, 307);
    }
    s->upload = data;
    s->upload_len = len;
    snprintf(s->upload_name, sizeof s->upload_name, "%s", name);
    nc_log("CH%02d: ファイル %s (%zu バイト) を受信", s->no, name, len);
    return out(s, "%s  %s %zu bytes\n", M(319), name, len);
}

/* 書いて保存する。書いたら 1、やめたら 0 */
static int write_msg(struct sess *s, int board, int reply_to, const int *to) {
    char title[MAX_TITLE_BYTES + 1] = "", def[MAX_TITLE_BYTES + 1] = "";
    char *body = NULL;
    int r;
    pthread_mutex_lock(&g_lock);
    bool prog = g_boards[board].type == BT_PROGRAM, esc = g_boards[board].esc || board == MAIL_BOARD;
    pthread_mutex_unlock(&g_lock);
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
            if (p) snprintf(def, sizeof def, "%s", p->title);
            pthread_mutex_unlock(&g_lock);
        }
        CHK(ask_title(s, title, sizeof title, def));
        if (!title[0]) return outm_nl(s, 185);
        CHK(outm_nl(s, 192));
        CHK(outm_nl(s, 193));
        if ((r = edit_body(s, &body, esc)) < 0) goto fail;
    }
    for (;;) {
        char a[16];
        if ((r = ask(s, 194, a, sizeof a, RL_UPPER)) < 0) goto fail;
        switch (a[0]) {
        case 'Y': {
            if (prog) {
                if ((r = receive_file(s)) < 0) goto fail;
                if (!s->upload) break;
            }
            struct msg m = {.board = board, .from = s->uid, .reply_to = reply_to};
            snprintf(m.title, sizeof m.title, "%s", title);
            if (to)
                for (int k = 0; k < MAX_MAIL_TO; k++) {
                    m.to[k] = to[k];
                    m.to_state[k] = to[k] ? 'n' : 0;
                }
            pthread_mutex_lock(&g_lock);
            if (prog) {
                m.fid = file_add(s->upload, s->upload_len);
                m.fsize = (long)s->upload_len;
                snprintf(m.fname, sizeof m.fname, "%s", s->upload_name);
            }
            int seq = msg_add(&m, body ? body : "");
            if (board == MAIL_BOARD) USER(s)->wm++;
            else if (prog) USER(s)->wp++;
            else USER(s)->wb++;
            db_save_users();
            pthread_mutex_unlock(&g_lock);
            if (prog) {
                free(s->upload);
                s->upload = NULL;
            }
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
        case 'E':
            /* 本文を最初から入れ直す */
            free(body);
            body = NULL;
            if ((r = outm_nl(s, 193)) < 0 || (r = edit_body(s, &body, esc)) < 0) goto fail;
            break;
        case 'C':
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
            if ((r = out(s, "Y:書き込む N:捨てる T:タイトルを変える E:本文を入れ直す C:表示 Q:書きかけで抜ける\n")) < 0)
                goto fail;
        }
    }
fail:
    free(body);
    return r;
}

/* ------------------------------------------------------------ ダウンロード・バッチ */

static int download(struct sess *s, int board, int seq, bool ymodem) {
    pthread_mutex_lock(&g_lock);
    struct msg *m = msg_get(board, seq);
    bool ok = m && m->fid;
    char name[16] = "";
    long fid = 0;
    time_t t = 0;
    if (ok) {
        snprintf(name, sizeof name, "%s", m->fname);
        fid = m->fid;
        t = m->t;
    }
    pthread_mutex_unlock(&g_lock);
    if (!ok) return outm_nl(s, 307);
    int proto = XS_YMODEM, r;
    if (!ymodem) {
        if ((r = yn(s, 315)) < 0) return r;
        proto = r ? XS_X1K : XS_X128;
    }
    CHK(outm_nl(s, proto == XS_X128 ? 316 : proto == XS_X1K ? 317 : 318));
    pthread_mutex_lock(&g_lock);
    size_t len;
    unsigned char *data = file_get(fid, &len);
    pthread_mutex_unlock(&g_lock);
    if (!data) return outm_nl(s, 320);
    online_set_place(s->no, "DOWNLOAD");
    struct xfile f = {name, data, len, t};
    r = xfer_send(s->t, proto, &f, 1);
    free(data);
    if (r < 0) return r;
    if (r == 0) {
        pthread_mutex_lock(&g_lock);
        if ((m = msg_get(board, seq))) m->dls++;
        db_save_msgs();
        pthread_mutex_unlock(&g_lock);
        nc_log("CH%02d: %s を送信", s->no, name);
    }
    return outm_nl(s, r == 0 ? 319 : r == XF_CANCEL ? 334 : 320);
}

static int batch_add(struct sess *s, int board, int seq) {
    for (int i = 0; i < s->nbatch; i++)
        if (s->batch[i].board == board && s->batch[i].seq == seq) return outm_nl(s, 322);
    if (s->nbatch >= MAX_BATCH) return outm_nl(s, 321);
    s->batch[s->nbatch].board = board;
    s->batch[s->nbatch].seq = seq;
    s->nbatch++;
    return outm_nl(s, 314);
}

static int batch_list(struct sess *s) {
    long total = 0;
    for (int i = 0; i < s->nbatch; i++) {
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(s->batch[i].board, s->batch[i].seq);
        char line[256] = "";
        if (m) {
            char fn[16];
            fname_83(m->fname, fn, sizeof fn);
            snprintf(line, sizeof line, "%2d:[%s] %5ldKB %s\n", i + 1, fn, (m->fsize + 1023) / 1024, m->title);
            total += (m->fsize + 1023) / 1024;
        }
        pthread_mutex_unlock(&g_lock);
        CHK(out(s, "%s", line));
    }
    return out(s, "%s%ld\n", M(327), total);
}

static int batch_send(struct sess *s) {
    struct xfile f[MAX_BATCH];
    unsigned char *bufs[MAX_BATCH];
    char names[MAX_BATCH][16];
    int n = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < s->nbatch; i++) {
        struct msg *m = msg_get(s->batch[i].board, s->batch[i].seq);
        if (!m || !m->fid) continue;
        size_t len;
        bufs[n] = file_get(m->fid, &len);
        if (!bufs[n]) continue;
        snprintf(names[n], sizeof names[n], "%s", m->fname);
        f[n] = (struct xfile){names[n], bufs[n], len, m->t};
        n++;
    }
    pthread_mutex_unlock(&g_lock);
    online_set_place(s->no, "BATCH");
    int r = n ? xfer_send(s->t, XS_YMODEM, f, n) : XF_FAIL;
    for (int i = 0; i < n; i++) free(bufs[i]);
    if (r == 0) {
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < s->nbatch; i++) {
            struct msg *m = msg_get(s->batch[i].board, s->batch[i].seq);
            if (m) m->dls++;
        }
        db_save_msgs();
        pthread_mutex_unlock(&g_lock);
        s->nbatch = 0;
    }
    return r;
}

int cmd_batch(struct sess *s, const char *arg) {
    if (!s->nbatch) return outm_nl(s, 326);
    CHK(batch_list(s));
    for (;;) {
        char a[16];
        CHK(ask(s, 323, a, sizeof a, RL_UPPER));
        switch (a[0]) {
        case 'Y':
        case 'O': {
            CHK(outm_nl(s, a[0] == 'Y' ? 328 : 333));
            int r = batch_send(s);
            if (r < 0) return r;
            CHK(outm_nl(s, r == 0 ? 332 : 331));
            if (a[0] == 'O' && r == 0) return T_DISCONNECT;
            return 0;
        }
        case 'N':
            return outm_nl(s, 324);
        case 'D': {
            int k;
            CHK(ask_int(s, 325, 0, &k));
            if (k >= 1 && k <= s->nbatch) {
                memmove(&s->batch[k - 1], &s->batch[k], sizeof s->batch[0] * (size_t)(s->nbatch - k));
                s->nbatch--;
            }
            if (!s->nbatch) return outm_nl(s, 326);
            break;
        }
        case 'L':
            CHK(batch_list(s));
            break;
        case 'C': {
            int r = yn(s, 329);
            if (r < 0) return r;
            if (r) {
                s->nbatch = 0;
                return outm_nl(s, 330);
            }
            break;
        }
        default:
            CHK(out(s, "Y:転送する O:転送して回線を切る N:抜ける D:リストから外す L:一覧 C:リストを空にする\n"));
        }
    }
}

/* ------------------------------------------------------------ メッセージの編集 (BOP・SYSOP) */

static int edit_msg(struct sess *s, int board, int seq) {
    char a[16];
    CHK(ask(s, 342, a, sizeof a, RL_UPPER));
    switch (a[0]) {
    case 'D': {
        int r = yn(s, 221);
        if (r <= 0) return r;
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(board, seq);
        if (m) m->deleted = true;
        db_save_msgs();
        pthread_mutex_unlock(&g_lock);
        return outm_nl(s, 204);
    }
    case 'T': {
        char title[MAX_TITLE_BYTES + 1];
        CHK(ask_title(s, title, sizeof title, NULL));
        if (!title[0]) return 0;
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(board, seq);
        if (m) snprintf(m->title, sizeof m->title, "%s", title);
        db_save_msgs();
        pthread_mutex_unlock(&g_lock);
        return outm_nl(s, 220);
    }
    case 'U': {
        int id;
        CHK(ask_int(s, 343, -1, &id));
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(board, seq);
        bool ok = m && user_get(id);
        if (ok) m->from = id;
        db_save_msgs();
        pthread_mutex_unlock(&g_lock);
        return ok ? outm_nl(s, 220) : outm_nl(s, 135);
    }
    case 'F': {
        char name[16];
        CHK(ask(s, 311, name, sizeof name, RL_UPPER));
        if (!name[0]) return 0;
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(board, seq);
        if (m) snprintf(m->fname, sizeof m->fname, "%s", name);
        db_save_msgs();
        pthread_mutex_unlock(&g_lock);
        return outm_nl(s, 220);
    }
    case 'S': {
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(board, seq);
        if (m && m->fid) {
            size_t len;
            free(file_get(m->fid, &len));
            m->fsize = (long)len;
            db_save_msgs();
        }
        pthread_mutex_unlock(&g_lock);
        return outm_nl(s, 220);
    }
    }
    return 0;
}

/* ------------------------------------------------------------ 読む */

static int delete_msg(struct sess *s, int board, int seq) {
    pthread_mutex_lock(&g_lock);
    struct msg *m = msg_get(board, seq);
    bool mine = m && (m->from == s->uid || IS_SYSOP(s) || (board != MAIL_BOARD && g_boards[board].bop == s->uid));
    bool mail = m && board == MAIL_BOARD && mail_is_for(m, s->uid, NULL);
    int replies = m ? m->replies : 0;
    pthread_mutex_unlock(&g_lock);
    if (!mine && !mail) return out(s, "==== 他の人のメッセージは削除できません ====\n");
    if (!mail && replies > 0 && !IS_SYSOP(s)) return out(s, "==== リプライが付いているので削除できません ====\n");
    int r = yn(s, mail ? 238 : 221);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    m = msg_get(board, seq);
    int slot;
    if (m && mail && mail_is_for(m, s->uid, &slot)) {
        m->to_state[slot] = 'd';
        bool all = true;
        for (int k = 0; k < MAX_MAIL_TO; k++)
            if (m->to[k] && m->to_state[k] != 'd') all = false;
        if (all) m->deleted = true;
    } else if (m) m->deleted = true;
    db_save_msgs();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 204);
}

static int read_help(struct sess *s) {
    return out(s, "R/Enter:読む V:最新まで続けて読む A/L:タイトル 20 件 (名前/ID) N/B:次/前 T/E:最初/最新\n"
                  "P:これにリプライ F:直前に読んだものにリプライ W:書く D:削除 M:回数 O:ボードオペ\n"
                  "U:リプライ先へ I:元の位置へ X/Y:XMODEM/YMODEM で受け取る S:バッチに登録 !:編集 番号:その番号 "
                  "Q:終わる\n");
}

/* 返信を書く (メールは送り主へ) */
static int reply(struct sess *s, int board, int seq);

/* mode: RB_NORMAL 対話 / RB_RALL 最後まで読んだら戻る / RB_NONSTOP 未読を止まらずに表示 */
int read_board(struct sess *s, int board, int mode) {
    pthread_mutex_lock(&g_lock);
    struct board *b = &g_boards[board];
    int n = board == MAIL_BOARD ? 0 : board_count(board);
    char head[256], place[16];
    snprintf(head, sizeof head, "\n-\t-\t-\t-\t-\t-\t-\t-\t-\t-\nNo.%d ｢%s｣\n", board, b->title);
    snprintf(place, sizeof place, "%s", b->index);
    bool prog = b->type == BT_PROGRAM;
    pthread_mutex_unlock(&g_lock);
    online_set_place(s->no, place);
    if (board == MAIL_BOARD) n = neighbor(s, board, 0, 1) ? 1 : 0;
    CHK(out(s, "%s", head));
    if (n == 0) return outm_nl(s, 197);

    int cur = first_unread(s, board), last = 0, back = 0;
    if (mode == RB_NONSTOP || (mode == RB_RALL && s->nonstop)) {
        for (; cur; cur = neighbor(s, board, cur, 1)) {
            CHK(show_header(s, board, cur));
            CHK(show_body(s, board, cur));
        }
        return 0;
    }
    if (cur) CHK(show_header(s, board, cur));
    else CHK(outm_nl(s, 198));
    for (;;) {
        char a[32];
        CHK(ask(s, 196, a, sizeof a, RL_UPPER));
        char c = isdigit((unsigned char)a[0]) ? '#' : a[0];
        int arg = isdigit((unsigned char)a[0]) ? atoi(a) : atoi(a + (a[0] ? 1 : 0));
        switch (c) {
        case 0:
        case 'R':
            if (!cur) {
                CHK(outm_nl(s, 198));
                if (mode == RB_RALL) return 0;
                break;
            }
            CHK(show_body(s, board, cur));
            last = cur;
            if (board == MAIL_BOARD) CHK(delete_msg(s, board, cur));
            cur = neighbor(s, board, cur, 1);
            if (cur) CHK(show_header(s, board, cur));
            else {
                CHK(outm_nl(s, 198));
                if (mode == RB_RALL) return 0;
            }
            break;
        case 'V':
            for (; cur; cur = neighbor(s, board, cur, 1)) {
                CHK(show_header(s, board, cur));
                CHK(show_body(s, board, cur));
                last = cur;
            }
            CHK(outm_nl(s, 198));
            if (mode == RB_RALL) s->nonstop = true;
            return 0;
        case 'A':
        case 'L': {
            int from = cur ? cur : neighbor(s, board, 0, 1), end;
            CHK(list_titles(s, board, from, c == 'A', &end));
            cur = end ? neighbor(s, board, end, 1) : 0;
            if (cur) CHK(show_header(s, board, cur));
            else CHK(outm_nl(s, 198));
            break;
        }
        case 'N':
        case 'B': {
            int to = c == 'N' ? (cur ? neighbor(s, board, cur, 1) : 0)
                              : neighbor(s, board, cur ? cur : 0x7fffffff, -1);
            if (!to) {
                CHK(outm_nl(s, c == 'N' ? 198 : 197));
                if (c == 'N') cur = 0;
                break;
            }
            cur = to;
            CHK(show_header(s, board, cur));
            break;
        }
        case 'T':
        case 'E':
            cur = c == 'T' ? neighbor(s, board, 0, 1) : neighbor(s, board, 0x7fffffff, -1);
            if (cur) CHK(show_header(s, board, cur));
            break;
        case '#':
            if (!exists(s, board, arg)) {
                CHK(outm_nl(s, 197));
                break;
            }
            cur = arg;
            CHK(show_header(s, board, cur));
            break;
        case 'P':
        case 'F': {
            int target = c == 'P' ? cur : last;
            if (!target) CHK(outm_nl(s, 197));
            else CHK(reply(s, board, target));
            break;
        }
        case 'W': {
            if (board == MAIL_BOARD) {
                CHK(cmd_mwrite(s, ""));
                break;
            }
            pthread_mutex_lock(&g_lock);
            bool w = board_can_write(USER(s), &g_boards[board]);
            pthread_mutex_unlock(&g_lock);
            if (!w) CHK(outm_nl(s, 183));
            else CHK(write_msg(s, board, 0, NULL));
            break;
        }
        case 'D':
            if (cur) CHK(delete_msg(s, board, cur));
            break;
        case 'M': {
            pthread_mutex_lock(&g_lock);
            struct msg *m = msg_get(board, cur);
            int reads = m ? m->reads : 0, dls = m ? m->dls : 0;
            pthread_mutex_unlock(&g_lock);
            CHK(out(s, "%s%d\n", M(371), reads));
            if (prog) CHK(out(s, "%s%d\n", M(372), dls));
            break;
        }
        case 'O': {
            pthread_mutex_lock(&g_lock);
            int op = g_boards[board].bop;
            char nm[64];
            snprintf(nm, sizeof nm, "%s", name_of(op));
            pthread_mutex_unlock(&g_lock);
            CHK(out(s, "%s%04d:%s%s\n", M(373), op, nm, M(374)));
            break;
        }
        case 'U': {
            pthread_mutex_lock(&g_lock);
            struct msg *m = msg_get(board, cur);
            int parent = m ? m->reply_to : 0;
            pthread_mutex_unlock(&g_lock);
            if (!parent || !exists(s, board, parent)) {
                CHK(outm_nl(s, 197));
                break;
            }
            back = cur;
            cur = parent;
            CHK(show_header(s, board, cur));
            break;
        }
        case 'I':
            if (back) {
                cur = back;
                back = 0;
                CHK(show_header(s, board, cur));
            }
            break;
        case 'X':
        case 'Y':
            if (!prog || !cur) CHK(outm_nl(s, 307));
            else CHK(download(s, board, cur, c == 'Y'));
            break;
        case 'S':
            if (!prog || !cur) CHK(outm_nl(s, 307));
            else CHK(batch_add(s, board, cur));
            break;
        case '!':
            pthread_mutex_lock(&g_lock);
            bool op = IS_SYSOP(s) || g_boards[board].bop == s->uid;
            pthread_mutex_unlock(&g_lock);
            if (op && cur) CHK(edit_msg(s, board, cur));
            else CHK(outm_nl(s, 145));
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

static int choose_and_read(struct sess *s, const char *arg) {
    int b;
    if (arg[0]) {
        pthread_mutex_lock(&g_lock);
        struct board *bp = board_by_index(arg[0] == '\\' ? arg + 1 : arg);
        b = bp && board_can_read(USER(s), bp) ? bp->no : -1;
        pthread_mutex_unlock(&g_lock);
        if (b < 0) return outm_nl(s, 335);
    } else {
        print_board_list(s, false);
        b = choose_board(s, 181, false);
        if (b < -1) return b + 1;
        if (b < 0) return 0;
    }
    int r = read_board(s, b, RB_NORMAL);
    pthread_mutex_lock(&g_lock);
    db_save_ptrs();
    db_save_msgs();
    pthread_mutex_unlock(&g_lock);
    return r;
}

int cmd_bread(struct sess *s, const char *arg) { return choose_and_read(s, arg); }

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
    char title[128];
    pthread_mutex_lock(&g_lock);
    snprintf(title, sizeof title, "%s", g_boards[b].title);
    pthread_mutex_unlock(&g_lock);
    CHK(out(s, "No.%d ｢%s｣\n", b, title));
    int r = write_msg(s, b, 0, NULL);
    return r < 0 ? r : 0;
}

/* 巡回: BSET で Y にした読めるボードを順に */
static int read_all(struct sess *s, int mode) {
    s->nonstop = false;
    for (int i = 0; i < MAX_BOARDS; i++) {
        pthread_mutex_lock(&g_lock);
        bool target = g_boards[i].used && *bset_flag(s->uid, i) == 'Y' &&
                      (i == MAIL_BOARD || (board_can_read(USER(s), &g_boards[i]) && board_unread(s->uid, i) > 0));
        pthread_mutex_unlock(&g_lock);
        if (target && i == MAIL_BOARD) target = first_unread(s, MAIL_BOARD) != 0;
        if (target) CHK(read_board(s, i, mode));
    }
    s->nonstop = false;
    pthread_mutex_lock(&g_lock);
    db_save_ptrs();
    db_save_msgs();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 199);
}

int cmd_rall(struct sess *s, const char *arg) { return read_all(s, RB_RALL); }
int cmd_rnall(struct sess *s, const char *arg) { return read_all(s, RB_NONSTOP); }

/* 読めるボードを順に出して、巡回の対象にするかを聞く (空 Enter は今のまま) */
int cmd_bset(struct sess *s, const char *arg) {
    for (int i = 0; i < MAX_BOARDS; i++) {
        pthread_mutex_lock(&g_lock);
        struct board *b = &g_boards[i];
        bool show = b->used && board_can_read(USER(s), b);
        char line[256] = "";
        char cur = *bset_flag(s->uid, i);
        if (show) snprintf(line, sizeof line, "%2d. \\%-10s %s [%c]\n", i, b->index, b->title, cur);
        pthread_mutex_unlock(&g_lock);
        if (!show) continue;
        CHK(out(s, "%s", line));
        char a[8];
        CHK(ask(s, 200, a, sizeof a, RL_UPPER));
        if (a[0] == 'Q') break;
        if (a[0] != 'Y' && a[0] != 'N') continue;
        pthread_mutex_lock(&g_lock);
        *bset_flag(s->uid, i) = a[0];
        pthread_mutex_unlock(&g_lock);
    }
    pthread_mutex_lock(&g_lock);
    db_save_ptrs();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

/* 既読位置をまとめて動かす */
int cmd_pmove(struct sess *s, const char *arg) {
    char a[16];
    CHK(ask(s, 287, a, sizeof a, RL_UPPER));
    time_t when = 0;
    char label[64];
    switch (a[0]) {
    case 'T': snprintf(label, sizeof label, "%s", M(288)); when = 0; break;
    case 'E': snprintf(label, sizeof label, "%s", M(289)); when = (time_t)-1; break;
    case 'L':
        pthread_mutex_lock(&g_lock);
        when = USER(s)->prev_login;
        pthread_mutex_unlock(&g_lock);
        fmt_time(when, label, sizeof label);
        break;
    case 'D': {
        char d[32], t[32];
        CHK(ask(s, 290, d, sizeof d, 0));
        CHK(ask(s, 291, t, sizeof t, 0));
        struct tm tm = {0};
        int y, mo, dd, h = 0, mi = 0;
        if (sscanf(d, "%d-%d-%d", &y, &mo, &dd) != 3 && sscanf(d, "%d/%d/%d", &y, &mo, &dd) != 3) return 0;
        sscanf(t, "%d:%d", &h, &mi);
        tm.tm_year = y < 100 ? (y < 70 ? y + 100 : y) : y - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = dd;
        tm.tm_hour = h;
        tm.tm_min = mi;
        tm.tm_isdst = -1;
        when = mktime(&tm);
        snprintf(label, sizeof label, "%s %s", d, t);
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
            if (m->board == b && m->t < when && m->seq > p) p = m->seq;
        }
        if (when == (time_t)-1) p = g_boards[b].next_seq - 1;
        *read_ptr(s->uid, b) = p;
    }
    db_save_ptrs();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 285);
}

/* ------------------------------------------------------------ メール */

int login_mail_notice(struct sess *s) {
    int n = 0, slot;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmsgs; i++) {
        struct msg *m = &g_msgs[i];
        if (m->board == MAIL_BOARD && !m->deleted && mail_is_for(m, s->uid, &slot) && m->to_state[slot] == 'n') n++;
    }
    pthread_mutex_unlock(&g_lock);
    if (!n) return 0;
    return out(s, "%smail%s%d%s\n", M(239), M(240), n, M(241));
}

/* 宛先を聞いてメールを書く。reply_from があればその人に返信 */
static int mail_to(struct sess *s, int reply_from, int reply_seq) {
    int to[MAX_MAIL_TO] = {0};
    int n = 0;
    if (reply_from) {
        pthread_mutex_lock(&g_lock);
        char buf[80];
        snprintf(buf, sizeof buf, "%s", name_of(reply_from));
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
        else if (u->mailbox_closed && USER(s)->level < LV_SIGOP) err = 234;
        else if (u->level < g_boards[MAIL_BOARD].rlevel) err = 242;
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
    int r = write_msg(s, MAIL_BOARD, reply_seq, to);
    return r < 0 ? r : 0;
}

static int reply(struct sess *s, int board, int seq) {
    if (board == MAIL_BOARD) {
        pthread_mutex_lock(&g_lock);
        struct msg *m = msg_get(board, seq);
        int from = m ? m->from : 0;
        pthread_mutex_unlock(&g_lock);
        return from ? mail_to(s, from, seq) : 0;
    }
    pthread_mutex_lock(&g_lock);
    bool w = board_can_write(USER(s), &g_boards[board]);
    pthread_mutex_unlock(&g_lock);
    if (!w) return outm_nl(s, 183);
    int r = write_msg(s, board, seq, NULL);
    return r < 0 ? r : 0;
}

static bool mail_allowed(struct sess *s, bool write) {
    pthread_mutex_lock(&g_lock);
    struct board *b = &g_boards[MAIL_BOARD];
    int lv = USER(s)->level;
    bool ok = lv >= (write ? b->wlevel : b->rlevel) && lv >= USER(s)->mail_level;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int cmd_mwrite(struct sess *s, const char *arg) {
    if (!mail_allowed(s, true)) return outm_nl(s, 354);
    return mail_to(s, 0, 0);
}

int cmd_mread(struct sess *s, const char *arg) {
    if (!mail_allowed(s, false)) return outm_nl(s, 355);
    int r = read_board(s, MAIL_BOARD, RB_NORMAL);
    pthread_mutex_lock(&g_lock);
    db_save_msgs();
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* 自分が出したメールを 1 通ずつ、宛先ごとの状態 (n 未読 / r 既読 / d 削除) を付けて出す */
int cmd_mcheck(struct sess *s, const char *arg) {
    int cur = 0;
    for (;;) {
        int seq = 0;
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_nmsgs; i++) {
            struct msg *m = &g_msgs[i];
            if (m->board == MAIL_BOARD && !m->deleted && m->from == s->uid && m->seq > cur && (!seq || m->seq < seq))
                seq = m->seq;
        }
        struct msg c = {0};
        struct msg *m = seq ? msg_get(MAIL_BOARD, seq) : NULL;
        if (m) c = *m;
        char to[320] = "";
        for (int k = 0; k < MAX_MAIL_TO; k++)
            if (c.to[k]) {
                size_t l = strlen(to);
                snprintf(to + l, sizeof to - l, " %04d:%s%s", c.to[k], name_of(c.to[k]),
                         M(c.to_state[k] == 'n' ? 362 : c.to_state[k] == 'r' ? 363 : 364));
            }
        pthread_mutex_unlock(&g_lock);
        if (!seq) break;
        char ts[32];
        fmt_time(c.t, ts, sizeof ts);
        CHK(out(s, "\nmail %s %s\n %s\n", ts, c.title, to));
        char a[16];
        for (bool next = false; !next;) {
            CHK(ask(s, 365, a, sizeof a, RL_UPPER));
            switch (a[0]) {
            case 0:
            case 'Y':
                next = true;
                break;
            case 'D': {
                int r = yn(s, 366);
                if (r < 0) return r;
                if (r) {
                    pthread_mutex_lock(&g_lock);
                    struct msg *mm = msg_get(MAIL_BOARD, seq);
                    if (mm) mm->deleted = true;
                    db_save_msgs();
                    pthread_mutex_unlock(&g_lock);
                    CHK(outm_nl(s, 367));
                }
                next = true;
                break;
            }
            case 'R': {
                pthread_mutex_lock(&g_lock);
                struct msg *mm = msg_get(MAIL_BOARD, seq);
                char *body = mm ? msg_body(mm) : strdup("");
                pthread_mutex_unlock(&g_lock);
                int r = out(s, "%s", body);
                free(body);
                CHK(r);
                break;
            }
            case 'Q':
                return outm_nl(s, 361);
            }
        }
        cur = seq;
    }
    return outm_nl(s, 361);
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
