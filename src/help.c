/*
 * ヘルプ・テンプレート (HELP.TXT)
 *
 * 画面の決まった部分 (ボードの見出し、メッセージのヘッダ、オープニング、UREAD、USTAT など) と、
 * 各モードのヘルプは、テンプレートから作る。NET-COCK の HELP.TXT を設定の help_file で指定すると
 * 元と同じ表示になる。指定しないときは、このファイルにある独自のテンプレートを使う。
 *
 * マクロ (/c が長い形、%c が短い形):
 *   値: " ボード名  # タイトル  $ リプライ先のタイトル  & 日付  ' 時刻  ( ファイル名  ) 発信者 ID  * 発信者名
 *       + , リプライ先の ID・名前  - インデックス  . リプライ先の番号  (空白) プログラムのサイズ  [ 総数
 *       \ 今の位置  ] ボード番号  ^ リプライ数  _ バイト数  0〜9 システムメッセージ  : 速度  ; エラー訂正
 *       < メールボックス  = チャットモード  > 新着メール  ? ログネーム  @ A 今回のログインの日付・時刻
 *       B C 前回のログインの日付・時刻  D 本日の時間  E 総時間  F G 今月・前月の時間  H 今回の時間
 *       I 本日の残り  J 1 日の持ち時間  K レベル  L 本日の回数  M 回線  N ログイン回数  O P 今月・前月の回数
 *       Q 全体のログイン回数  R 全体のゲストログイン回数  S ID  T U V 書き込み数  W ID の発行日
 *       n 公開住所  o 職業  p 機種  q 自己紹介  r 登録住所  s 氏名  t UREAD の ID  { パスワードミスの回数
 *       | スケジュール  ` X のアカウント (独自)  aN bN cN 回線 N の本日・昨日・累計の時間  dN 昨日の使用率  eMN 回線 M〜N の平均
 *       fN gN hN 回数 (N = * は全回線)  i j アクセスが無かった時間 (本日・昨日)  k l BUSY 時間
 *   /m    以後の会員データを UREAD の相手のものにする
 *   条件: X リプライ  Y メール  Z プログラム  } ルート  ~ タイトルだけ  u 仮会員  v 一般  w Sigop
 *         x Sysop  y 会員管理者  z パスワードミスがあった。/c は真のとき、%c は偽のときに %% まで出す
 *   %/ 改行せずに終わる  // は /  /% は %
 */
#include "help.h"

#include <ctype.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBLOCK 15
static char *loaded[NBLOCK];

static const char *const fallback[NBLOCK] = {
    /* 0 ボード名の見出し */
    "-\t-\t-\t-\t-\t-\t-\t-\t-\t-\nNo.%] ｢%\"｣\n",
    /* 1 リプライ先 */
    "[re:%. %$//%+:%,]\n",
    /* 2 メッセージのヘッダ */
    "%Y/-(%\\//%[) %& %' %#\n%%/Ymail %& %' %#\n%%/Z  [/(] % KB /):%*%%%Z  %_bytes /):%*%%%/",
    /* 3 ゲストのオープニング */
    "/1\n/3\n/5\n",
    /* 4 会員のオープニング */
    "/1\n/2\n\n/S:%?  前回 %B %C  %N 回目\n/zパスワードの入力ミスが %{ 回ありました。\n%%/>/|/=",
    /* 5 ログアウト */
    "/4\n/H (%H 秒)\n",
    /* 6 UREAD */
    "/mID        : /S  %K\nログネーム: %?\n住所      : %n\n職業      : %o\n機種      : %p\n自己紹介  : %q\n"
    "X         : %`\n最終接続  : %B %C\n総使用時間: /E  ログイン %N 回\n書き込み  : ボード %T / メール %U / プログラム %V\n",
    /* 7 USTAT */
    "今回のログイン : /@ /A (前回 /B /C)\n利用時間       : 今回 /H / 本日 /D / 今月 /F / 前月 /G / 合計 /E\n"
    "持ち時間       : 本日の残り %I 分 (1 日 %J 分)\nログイン回数   : 今月 %O / 前月 %P / 合計 %N\n"
    "書き込み       : ボード %T / メール %U / プログラム %V\n回線 %M  速度 %:  ID の発行 /W  レベル %K\n"
    "メールボックス : %<\n",
    /* 8 ACCESS (NULL のときは回線数に合わせて作る) */
    NULL,
    /* 9 チャットのヘルプ */
    ".S[回線] 送受信 ON / .O[回線] OFF / .R[回線] 受信だけ / .C シスオペを呼ぶ / .T 右寄せ / .B 中央寄せ\n"
    ".X 設定 / .Pn 回線 n だけに送る / .U回線 ローカル送出先 / .A ローカルだけに送る / .L 回線一覧 / .Q 抜ける\n",
    /* 10 書き込みのヘルプ */
    "Y:書き込む N:捨てる T:タイトルを変える E:本文を入れ直す C:表示 Q:書きかけで抜ける\n",
    /* 11 読み出しのヘルプ */
    "R/Enter:読む V:最新まで続けて読む A/L:タイトル 20 件 (名前/ID) N/B:次/前 T/E:最初/最新\n"
    "P:これにリプライ F:直前に読んだものにリプライ W:書く D:削除 M:回数 O:ボードオペ\n"
    "U:リプライ先へ I:元の位置へ X/Y/Z:XMODEM/YMODEM/ZMODEM で受け取る S:バッチに登録 !:編集 番号:その番号 Q:終わる\n",
    /* 12 バッチのヘルプ */
    "Y:転送する Z:ZMODEM で転送する O:転送して回線を切る N:抜ける D:リストから外す L:一覧 C:リストを空にする\n",
    /* 13 自動発行の案内 */
    "入会の申し込みです。聞かれた項目に全部答えてください。\n"
    "すぐに仮 ID を発行します。内容を確かめてから正会員にします。\n",
    /* 14 手作業発行の案内 */
    "入会の申し込みです。聞かれた項目に全部答えてください。ID の発行までしばらくお待ちください。\n",
};

/* 旧版 (6 ブロック: チャット・書き込み・読み出し・バッチ・自動発行・手作業発行) の並び */
static const int old_order[6] = {9, 10, 11, 12, 13, 14};

static char *sjis_to_utf8(const char *in) {
    iconv_t cd = iconv_open("UTF-8", "CP932");
    size_t il = strlen(in), ol = il * 3 + 1;
    char *out = calloc(1, ol), *op = out, *ip = (char *)in;
    if (cd != (iconv_t)-1) {
        iconv(cd, &ip, &il, &op, &ol);
        iconv_close(cd);
    }
    return out;
}

int help_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *blocks[NBLOCK * 2] = {0};
    int n = 0;
    bool in_comment = true;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '!' && line[1] == '!') break;
        if (line[0] == '*') {
            in_comment = true;
            continue;
        }
        if (in_comment) {
            if (n >= NBLOCK * 2) break;
            blocks[n++] = calloc(1, 1);
            in_comment = false;
        }
        char *u = sjis_to_utf8(line);
        size_t l = strlen(blocks[n - 1]);
        blocks[n - 1] = realloc(blocks[n - 1], l + strlen(u) + 2);
        sprintf(blocks[n - 1] + l, "%s\n", u);
        free(u);
    }
    fclose(f);
    for (int i = 0; i < n; i++) {
        int id = n == 6 ? old_order[i] : i;
        if (id < NBLOCK && (n == 6 || n >= NBLOCK)) {
            free(loaded[id]);
            loaded[id] = blocks[i];
        } else free(blocks[i]);
    }
    return n;
}

const char *help_block(int id) {
    if (id < 0 || id >= NBLOCK) return NULL;
    return loaded[id] ? loaded[id] : fallback[id];
}

/* ------------------------------------------------------------ 展開 */

struct buf {
    char *p;
    size_t len, cap;
};

static void put(struct buf *b, const char *s) {
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
}

static void day_str(time_t t, bool lng, char *o, size_t sz) {
    if (!t) snprintf(o, sz, "-");
    else strftime(o, sz, lng ? "%Y/%m/%d" : "%y/%m/%d", localtime(&t));
}

static void time_str(time_t t, bool lng, char *o, size_t sz) {
    if (!t) snprintf(o, sz, "-");
    else strftime(o, sz, lng ? "%H:%M:%S" : "%H:%M", localtime(&t));
}

static void dur_str(long sec, bool lng, char *o, size_t sz) {
    if (lng) snprintf(o, sz, "%ld:%02ld:%02ld", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(o, sz, "%ld", sec);
}

/* 回線ごとの統計 (ログから数える) */
struct stat_line {
    long t_today, t_yest, t_all;
    int c_today, c_yest, c_all;
};

static void line_stats(int line, struct stat_line *st, time_t *oldest) {
    enum { MAXLOG = 512 };
    static struct logent e[MAXLOG];
    memset(st, 0, sizeof *st);
    int n = log_all(e, MAXLOG);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    time_t today = mktime(&tm), yest = today - 86400;
    *oldest = n ? e[0].in : now;
    for (int i = 0; i < n; i++) {
        if (line > 0 && e[i].line != line) continue;
        long d = (long)(e[i].out - e[i].in);
        st->t_all += d, st->c_all++;
        if (e[i].in >= today) st->t_today += d, st->c_today++;
        else if (e[i].in >= yest) st->t_yest += d, st->c_yest++;
    }
}

/* 回線番号 (数字か *) を読む。*p を進める。* は 0 */
static int read_line_no(const char **p) {
    if (**p == '*') {
        (*p)++;
        return 0;
    }
    int n = 0;
    while (isdigit((unsigned char)**p)) n = n * 10 + (*(*p)++ - '0');
    return n;
}

static bool cond(const struct help_ctx *c, const struct user *u, char k) {
    const struct msg *m = c->m;
    switch (k) {
    case 'X': return m && m->reply_to;
    case 'Y': return c->board == MAIL_BOARD;
    case 'Z': return c->board > 0 && g_boards[c->board].type == BT_PROGRAM;
    case '}': return m && !m->reply_to;
    case '~': return m && m->len == 0;
    case 'u': return u && u->level > 0 && u->level < g_sys.user_level;
    case 'v': return u && u->level >= g_sys.user_level && u->level < LV_SIGOP;
    case 'w': return u && u->level >= LV_SIGOP && u->level < LV_SYSOP;
    case 'x': return u && u->level >= LV_SYSOP;
    case 'y': return u && u->id == g_sys.manager;
    case 'z': return u && u->pass_miss > 0;
    }
    return false;
}

/* 値のマクロ。対応しないものは false */
static bool value(const struct help_ctx *c, const struct user *u, char k, bool lng, const char **pp, char *o,
                  size_t sz) {
    const struct msg *m = c->m;
    const struct board *b = c->board >= 0 ? &g_boards[c->board] : NULL;
    const struct msg *re = m && m->reply_to ? msg_get(m->board, m->reply_to) : NULL;
    const struct online *ol = c->s ? &g_online[c->s->no] : NULL;
    time_t now = time(NULL);
    o[0] = 0;
#define NUM(v) snprintf(o, sz, lng ? "%5ld" : "%ld", (long)(v))
#define ID(v) snprintf(o, sz, lng ? "%04d" : "%d", (int)(v))
#define STR(v) snprintf(o, sz, "%s", (v))
    switch (k) {
    case '"': if (b) STR(b->title); return true;
    case '#': if (m) STR(m->title); return true;
    case '$': if (re) STR(re->title); return true;
    case '&': if (m) day_str(m->t, lng, o, sz); return true;
    case '\'': if (m) time_str(m->t, lng, o, sz); return true;
    case '(':
        if (m && lng) {
            /* 「NAME    .EXT」の形に揃える */
            const char *dot = strrchr(m->fname, '.');
            int bl = dot ? (int)(dot - m->fname) : (int)strlen(m->fname);
            snprintf(o, sz, "%-8.*s.%-3s", bl > 8 ? 8 : bl, m->fname, dot ? dot + 1 : "");
        } else if (m) STR(m->fname);
        return true;
    case ')': if (m) ID(m->from); return true;
    case '*': if (m) { const struct user *f = user_get(m->from); STR(f ? f->logname : "(削除)"); } return true;
    case '+': if (re) ID(re->from); return true;
    case ',': if (re) { const struct user *f = user_get(re->from); STR(f ? f->logname : "(削除)"); } return true;
    case '-':
        if (b) {
            STR(b->index);
            if (!lng)
                for (char *q = o; *q; q++) *q = (char)tolower((unsigned char)*q);
        }
        return true;
    case '.': if (m) snprintf(o, sz, "%d", m->reply_to); return true;
    case ' ': if (m) NUM((m->fsize + 1023) / 1024); return true;
    case '[': NUM(c->total); return true;
    case '\\': NUM(c->ptr); return true;
    case ']': if (c->board >= 0) snprintf(o, sz, "%d", c->board); return true;
    case '^': if (m) NUM(m->replies); return true;
    case '_': if (m) NUM(m->len); return true;
    case ':': STR(c->speed ? c->speed : "TCP"); return true;
    case ';': STR(c->speed && strchr(c->speed, '/') ? strchr(c->speed, '/') + 1 : "-"); return true;
    case '<': if (u) STR(u->mailbox_closed ? M(375) : M(376)); return true;
    case '=': STR(ol ? M(ol->chat ? 151 : 150) : ""); if (*o) strncat(o, "\n", sz - strlen(o) - 1); return true;
    case '>': STR(c->mail_notice ? c->mail_notice : ""); return true;
    case '|': STR(c->sched ? c->sched : ""); return true;
    case '?': if (u) STR(u->logname); return true;
    case '@': day_str(c->login_at, lng, o, sz); return true;
    case 'A': time_str(c->login_at, lng, o, sz); return true;
    case 'B': if (u) day_str(u->prev_login, lng, o, sz); return true;
    case 'C': if (u) time_str(u->prev_login, lng, o, sz); return true;
    case 'D': if (u) dur_str(u->today_sec + (c->login_at ? now - c->login_at : 0), lng, o, sz); return true;
    case 'E': if (u) dur_str(u->total_sec + (c->login_at ? now - c->login_at : 0), lng, o, sz); return true;
    case 'F': if (u) dur_str(u->month_sec + (c->login_at ? now - c->login_at : 0), lng, o, sz); return true;
    case 'G': if (u) dur_str(u->prev_month_sec, lng, o, sz); return true;
    case 'H': dur_str(c->login_at ? now - c->login_at : 0, lng, o, sz); return true;
    case 'I': if (ol) snprintf(o, sz, "%d", ol->unlimited ? 0 : ol->left / 60); return true;
    case 'J': if (u) snprintf(o, sz, "%d", u->day_minutes); return true;
    case 'K': if (u) { if (lng) snprintf(o, sz, "%d", u->level); else STR(level_name(u->level)); } return true;
    case 'L': if (u) NUM(u->logins_month); return true;
    case 'M': if (c->s) snprintf(o, sz, "%d", c->s->no); return true;
    case 'N': if (u) NUM(u->logins); return true;
    case 'O': if (u) NUM(u->logins_month); return true;
    case 'P': if (u) NUM(u->logins_prev); return true;
    case 'Q': NUM(g_sys.total_logins); return true;
    case 'R': NUM(g_sys.guest_logins); return true;
    case 'S': if (u) { if (lng) snprintf(o, sz, "%s%04d", g_sys.net_id, u->id); else ID(u->id); } return true;
    case 'T': if (u) NUM(u->wb); return true;
    case 'U': if (u) NUM(u->wm); return true;
    case 'V': if (u) NUM(u->wp); return true;
    case 'W': if (u) day_str(u->issued, lng, o, sz); return true;
    case 'n': if (u) STR(u->addr_pub); return true;
    case 'o': if (u) STR(u->job); return true;
    case 'p': if (u) STR(u->machine); return true;
    case 'q': if (u) STR(u->intro); return true;
    case 'r': if (u) STR(u->addr_priv); return true;
    case 's': if (u) STR(u->name); return true;
    case 't': if (c->target) ID(c->target->id); return true;
    case '{': if (u) snprintf(o, sz, "%d", u->pass_miss); return true;
    case '`': if (u && u->x_account[0]) snprintf(o, sz, "@%s", u->x_account); return true; /* 独自: X のアカウント */
    case 'a': case 'b': case 'c': case 'd': case 'f': case 'g': case 'h': {
        int line = read_line_no(pp);
        struct stat_line st;
        time_t oldest;
        line_stats(line, &st, &oldest);
        if (k == 'a') dur_str(st.t_today, lng, o, sz);
        else if (k == 'b') dur_str(st.t_yest, lng, o, sz);
        else if (k == 'c') dur_str(st.t_all, lng, o, sz);
        else if (k == 'd') snprintf(o, sz, "%.1f", st.t_yest * 100.0 / 86400);
        else NUM(k == 'f' ? st.c_today : k == 'g' ? st.c_yest : st.c_all);
        return true;
    }
    case 'e': {
        /* 回線 M〜N (1 桁ずつ) の平均使用率 */
        int m1 = isdigit((unsigned char)**pp) ? *(*pp)++ - '0' : 1;
        int m2 = isdigit((unsigned char)**pp) ? *(*pp)++ - '0' : m1;
        double sum = 0;
        int lines = 0;
        for (int l = m1; l <= m2; l++, lines++) {
            struct stat_line st;
            time_t oldest;
            line_stats(l, &st, &oldest);
            double days = (double)(now - oldest) / 86400.0;
            sum += st.t_all * 100.0 / (86400 * (days < 1 ? 1 : days));
        }
        snprintf(o, sz, "%.1f", lines ? sum / lines : 0.0);
        return true;
    }
    case 'i': case 'j': {
        struct stat_line st;
        time_t oldest;
        line_stats(0, &st, &oldest);
        struct tm tm;
        localtime_r(&now, &tm);
        long since_midnight = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
        long idle = k == 'i' ? since_midnight - st.t_today : 86400 - st.t_yest;
        dur_str(idle < 0 ? 0 : idle, lng, o, sz);
        return true;
    }
    case 'k': dur_str(g_sys.busy_sec, lng, o, sz); return true;
    case 'l': dur_str(0, lng, o, sz); return true;
    }
#undef NUM
#undef ID
#undef STR
    if (k >= '0' && k <= '9') {
        snprintf(o, sz, "%s", g_sys.sysmes[k - '0']);
        return true;
    }
    return false;
}

/* g_lock を持って呼ぶ。malloc した文字列を返す */
char *help_expand(int id, const struct help_ctx *c) {
    struct buf b = {0};
    put(&b, "");
    const char *t = help_block(id);
    if (!t) return b.p;
    const struct user *me = c->s && c->s->uid >= 0 ? &g_users[c->s->uid] : NULL;
    const struct user *u = me;
    bool skip = false, nl = true;
    for (const char *p = t; *p;) {
        char ch = *p;
        if ((ch == '/' || ch == '%') && p[1]) {
            char k = p[1];
            bool lng = ch == '/';
            p += 2;
            if (k == '%' && ch == '%') { /* %% 条件の終わり */
                skip = false;
                continue;
            }
            if (k == '/' && ch == '%') { /* %/ 改行せずに終わる */
                nl = false;
                break;
            }
            if (k == '/' && ch == '/') { if (!skip) put(&b, "/"); continue; }
            if (k == '%' && ch == '/') { if (!skip) put(&b, "%"); continue; }
            if (k == 'm' && lng) {
                u = c->target ? c->target : me;
                continue;
            }
            if (strchr("XYZ}~uvwxyz", k)) {
                if (cond(c, u, k) != lng) skip = true;
                continue;
            }
            char v[1100];
            if (value(c, u, k, lng, &p, v, sizeof v)) {
                if (!skip) put(&b, v);
                continue;
            }
            /* 知らないマクロはそのまま出す */
            char raw[3] = {ch, k, 0};
            if (!skip) put(&b, raw);
            continue;
        }
        size_t l = strcspn(p, "/%");
        if (!skip) {
            char *seg = strndup(p, l);
            put(&b, seg);
            free(seg);
        }
        p += l;
    }
    /* 最後の改行を %/ で消したとき */
    if (!nl) {
        while (b.len && b.p[b.len - 1] == '\n') b.p[--b.len] = 0;
    }
    return b.p;
}

int help_show(struct sess *s, int id, const struct help_ctx *c) {
    pthread_mutex_lock(&g_lock);
    char *text = help_expand(id, c);
    pthread_mutex_unlock(&g_lock);
    int r = out(s, "%s", text);
    free(text);
    return r;
}

bool help_has(int id) { return help_block(id) != NULL; }
