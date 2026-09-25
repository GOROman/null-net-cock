/*
 * データの読み書き (SQLite)
 *
 * 起動時に全部メモリに読み込み、変更のたびに SQLite に書き戻す。
 * 表の列は「項目名・型・構造体の中の位置」の表から作る。
 */
#include "db.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct user *g_users;
int g_nusers;
struct board g_boards[MAX_BOARDS];
struct msg *g_msgs;
int g_nmsgs;
static int msgs_cap;
struct system g_sys;

static sqlite3 *db;
static int *g_ptrs; /* [MAX_USERS][MAX_BOARDS] */
static char *g_bset;
static char *g_cug;
static struct logent *g_log;
static int log_n, log_head;
#define LOG_MAX 512
static struct application *g_apps;
static int apps_n, apps_cap;

/* ------------------------------------------------------------ 項目の表 */

enum ftype { F_INT, F_LONG, F_BOOL, F_CHAR, F_STR };
struct field {
    const char *name;
    enum ftype type;
    size_t off, size;
};
#define FLD(st, m, t) {#m, t, offsetof(struct st, m), sizeof(((struct st *)0)->m)}
#define FLDN(st, name, m, t) {name, t, offsetof(struct st, m), sizeof(((struct st *)0)->m)}

static const struct field user_fields[] = {
    FLD(user, id, F_INT), FLD(user, flags, F_INT), FLD(user, level, F_INT), FLD(user, day_minutes, F_INT),
    FLD(user, today_left, F_INT), FLD(user, mail_level, F_INT), FLD(user, logname, F_STR), FLD(user, sex, F_STR),
    FLD(user, kana, F_STR), FLD(user, name, F_STR), FLD(user, tel, F_STR), FLD(user, zip, F_STR),
    FLD(user, addr_priv, F_STR), FLD(user, addr_pub, F_STR), FLD(user, job, F_STR), FLD(user, machine, F_STR),
    FLD(user, pass, F_STR), FLD(user, birth, F_STR), FLD(user, intro, F_STR), FLD(user, applied, F_LONG),
    FLD(user, issued, F_LONG), FLD(user, last_login, F_LONG), FLD(user, prev_login, F_LONG), FLD(user, wb, F_INT),
    FLD(user, wm, F_INT), FLD(user, wp, F_INT), FLD(user, logins, F_INT), FLD(user, logins_month, F_INT),
    FLD(user, logins_prev, F_INT), FLD(user, total_sec, F_LONG), FLD(user, month_sec, F_LONG),
    FLD(user, prev_month_sec, F_LONG), FLD(user, today_sec, F_LONG), FLD(user, pass_miss, F_INT),
    FLD(user, esc, F_BOOL), FLD(user, bs_one_col, F_BOOL), FLD(user, menu, F_BOOL), FLD(user, chat_on_login, F_BOOL),
    FLD(user, mailbox_closed, F_BOOL), FLD(user, secret, F_BOOL),
};

static const struct field board_fields[] = {
    FLD(board, no, F_INT), FLD(board, bop, F_INT), FLD(board, rlevel, F_INT), FLD(board, wlevel, F_INT),
    FLD(board, group, F_INT), FLD(board, age_min, F_INT), FLD(board, age_max, F_INT), FLD(board, keep, F_INT),
    FLD(board, sex, F_INT), FLD(board, type, F_CHAR), FLD(board, esc, F_BOOL), FLD(board, cug, F_BOOL),
    FLD(board, timelimit, F_BOOL), FLD(board, index, F_STR), FLD(board, title, F_STR), FLD(board, next_seq, F_INT),
};

static const struct field msg_fields[] = {
    FLDN(msg, "id", off, F_LONG), FLD(msg, board, F_INT), FLD(msg, seq, F_INT), FLD(msg, t, F_LONG),
    FLD(msg, title, F_STR), FLDN(msg, "sender", from, F_INT), FLD(msg, reply_to, F_INT), FLD(msg, replies, F_INT),
    FLD(msg, len, F_LONG), FLD(msg, reads, F_INT), FLD(msg, dls, F_INT), FLD(msg, deleted, F_BOOL),
    FLD(msg, fname, F_STR), FLD(msg, fsize, F_LONG), FLD(msg, fid, F_LONG),
    FLDN(msg, "to0", to[0], F_INT), FLDN(msg, "to1", to[1], F_INT), FLDN(msg, "to2", to[2], F_INT),
    FLDN(msg, "to3", to[3], F_INT), FLDN(msg, "st0", to_state[0], F_CHAR), FLDN(msg, "st1", to_state[1], F_CHAR),
    FLDN(msg, "st2", to_state[2], F_CHAR), FLDN(msg, "st3", to_state[3], F_CHAR),
};

static const struct field app_fields[] = {
    FLD(application, no, F_INT), FLD(application, t, F_LONG), FLD(application, name, F_STR),
    FLD(application, kana, F_STR), FLD(application, addr, F_STR), FLD(application, zip, F_STR),
    FLD(application, tel, F_STR), FLD(application, pass, F_STR), FLD(application, issued_id, F_INT),
};

static const struct field log_fields[] = {
    FLDN(logent, "t_in", in, F_LONG), FLDN(logent, "t_out", out, F_LONG), FLD(logent, line, F_INT),
    FLD(logent, id, F_INT), FLD(logent, logname, F_STR), FLD(logent, speed, F_STR),
};

#define NF(a) (int)(sizeof a / sizeof a[0])

static void exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        nc_log("SQLite: %s (%s)", err ? err : "?", sql);
        sqlite3_free(err);
    }
}

static void create_table(const char *table, const struct field *f, int n, const char *extra) {
    char sql[4096];
    int k = snprintf(sql, sizeof sql, "CREATE TABLE IF NOT EXISTS %s (", table);
    for (int i = 0; i < n; i++)
        k += snprintf(sql + k, sizeof sql - (size_t)k, "%s\"%s\" %s", i ? ", " : "", f[i].name,
                      f[i].type == F_STR ? "TEXT" : "INTEGER");
    snprintf(sql + k, sizeof sql - (size_t)k, "%s)", extra ? extra : "");
    exec(sql);
    /* 古いデータベースに無い列を足す */
    for (int i = 0; i < n; i++) {
        snprintf(sql, sizeof sql, "SELECT \"%s\" FROM %s LIMIT 0", f[i].name, table);
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_finalize(st);
            continue;
        }
        snprintf(sql, sizeof sql, "ALTER TABLE %s ADD COLUMN \"%s\" %s DEFAULT %s", table, f[i].name,
                 f[i].type == F_STR ? "TEXT" : "INTEGER", f[i].type == F_STR ? "''" : "0");
        exec(sql);
    }
}

static void bind_fields(sqlite3_stmt *st, const struct field *f, int n, const void *rec) {
    const char *p = rec;
    for (int i = 0; i < n; i++) {
        const void *v = p + f[i].off;
        switch (f[i].type) {
        case F_INT: sqlite3_bind_int(st, i + 1, *(const int *)v); break;
        case F_LONG: sqlite3_bind_int64(st, i + 1, *(const long *)v); break;
        case F_BOOL: sqlite3_bind_int(st, i + 1, *(const bool *)v); break;
        case F_CHAR: sqlite3_bind_int(st, i + 1, *(const char *)v); break;
        case F_STR: sqlite3_bind_text(st, i + 1, v, -1, SQLITE_TRANSIENT); break;
        }
    }
}

static void read_fields(sqlite3_stmt *st, const struct field *f, int n, void *rec) {
    char *p = rec;
    for (int i = 0; i < n; i++) {
        void *v = p + f[i].off;
        switch (f[i].type) {
        case F_INT: *(int *)v = sqlite3_column_int(st, i); break;
        case F_LONG: *(long *)v = (long)sqlite3_column_int64(st, i); break;
        case F_BOOL: *(bool *)v = sqlite3_column_int(st, i) != 0; break;
        case F_CHAR: *(char *)v = (char)sqlite3_column_int(st, i); break;
        case F_STR: {
            const unsigned char *s = sqlite3_column_text(st, i);
            snprintf(v, f[i].size, "%s", s ? (const char *)s : "");
            break;
        }
        }
    }
}

/* 表を丸ごと書き直す。keep(rec) が false のレコードは書かない */
static void save_table(const char *table, const struct field *f, int n, const void *arr, size_t stride, int count,
                       bool (*keep)(const void *)) {
    char sql[2048];
    exec("BEGIN");
    snprintf(sql, sizeof sql, "DELETE FROM %s", table);
    exec(sql);
    int k = snprintf(sql, sizeof sql, "INSERT INTO %s VALUES (", table);
    for (int i = 0; i < n; i++) k += snprintf(sql + k, sizeof sql - (size_t)k, "%s?", i ? "," : "");
    snprintf(sql + k, sizeof sql - (size_t)k, ")");
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        for (int r = 0; r < count; r++) {
            const void *rec = (const char *)arr + stride * (size_t)r;
            if (keep && !keep(rec)) continue;
            bind_fields(st, f, n, rec);
            sqlite3_step(st);
            sqlite3_reset(st);
        }
        sqlite3_finalize(st);
    }
    exec("COMMIT");
}

/* 表を 1 行ずつ読む。each(rec) に渡す */
static void load_table(const char *table, const struct field *f, int n, size_t recsize, const char *order,
                       void (*each)(void *)) {
    char sql[2048];
    int k = snprintf(sql, sizeof sql, "SELECT ");
    for (int i = 0; i < n; i++) k += snprintf(sql + k, sizeof sql - (size_t)k, "%s\"%s\"", i ? "," : "", f[i].name);
    snprintf(sql + k, sizeof sql - (size_t)k, " FROM %s %s", table, order ? order : "");
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    void *rec = malloc(recsize);
    while (sqlite3_step(st) == SQLITE_ROW) {
        memset(rec, 0, recsize);
        read_fields(st, f, n, rec);
        each(rec);
    }
    free(rec);
    sqlite3_finalize(st);
}

/* ------------------------------------------------------------ 会員 */

static bool user_exists(const void *r) {
    const struct user *u = r;
    return u->id == (int)(u - g_users) && (u->id <= 1 || u->logname[0] || (u->flags & (UF_NEW | UF_DELETED)));
}
void db_save_users(void) {
    save_table("users", user_fields, NF(user_fields), g_users, sizeof *g_users, g_nusers, user_exists);
}
static void each_user(void *r) {
    struct user *u = r;
    if (u->id < 0 || u->id >= MAX_USERS) return;
    g_users[u->id] = *u;
    if (u->id + 1 > g_nusers) g_nusers = u->id + 1;
}

struct user *user_get(int id) {
    if (id < 0 || id >= g_nusers) return NULL;
    struct user *u = &g_users[id];
    if (u->id != id || (u->flags & UF_DELETED)) return NULL;
    if (id > 1 && !u->logname[0] && !(u->flags & UF_NEW)) return NULL;
    return u;
}

struct user *user_find(const char *s) {
    char *end;
    long id = strtol(s, &end, 10);
    if (*s && *end == 0) return user_get((int)id);
    /* ネットワーク ID 付き (例 COCK0010) */
    size_t nl = strlen(g_sys.net_id);
    if (nl && strncasecmp(s, g_sys.net_id, nl) == 0 && s[nl]) {
        id = strtol(s + nl, &end, 10);
        if (*end == 0) return user_get((int)id);
    }
    for (int i = 0; i < g_nusers; i++) {
        struct user *u = user_get(i);
        if (u && u->logname[0] && strcasecmp(u->logname, s) == 0) return u;
    }
    return NULL;
}

/* 新しい ID を発行する (空いている一番小さい番号。0 と 1 は使わない) */
int user_new(int level) {
    int id = -1;
    for (int i = 2; i < MAX_USERS; i++)
        if (i >= g_nusers || g_users[i].id != i) {
            id = i;
            break;
        }
    if (id < 0) return -1;
    struct user *u = &g_users[id];
    memset(u, 0, sizeof *u);
    u->id = id;
    u->level = level;
    u->flags = UF_NEW;
    u->day_minutes = u->today_left = g_sys.user_minutes;
    u->issued = time(NULL);
    u->bs_one_col = true;
    u->chat_on_login = true;
    if (id + 1 > g_nusers) g_nusers = id + 1;
    for (int b = 0; b < MAX_BOARDS; b++) {
        *bset_flag(id, b) = 'Y';
        *read_ptr(id, b) = 0;
        *cug_right(id, b) = 'n';
    }
    return id;
}

const char *level_name(int level) {
    if (level >= LV_SYSOP) return "Sysop.";
    if (level >= LV_SIGOP) return "Sigop.";
    if (level >= g_sys.user_level) return "一般";
    if (level > 0) return "仮会員";
    return "ゲスト";
}

/* ------------------------------------------------------------ ボード */

static bool board_used(const void *r) { return ((const struct board *)r)->used; }
void db_save_boards(void) {
    save_table("boards", board_fields, NF(board_fields), g_boards, sizeof *g_boards, MAX_BOARDS, board_used);
}
static void each_board(void *r) {
    struct board *b = r;
    if (b->no < 0 || b->no >= MAX_BOARDS) return;
    b->used = true;
    g_boards[b->no] = *b;
}

struct board *board_by_index(const char *idx) {
    char *end;
    long no = strtol(idx, &end, 10);
    if (*idx && *end == 0) return no > 0 && no < MAX_BOARDS && g_boards[no].used ? &g_boards[no] : NULL;
    /* 前方一致で省略できる。メール (0 番) は対象外 */
    size_t n = strlen(idx);
    for (int i = 1; i < MAX_BOARDS; i++)
        if (g_boards[i].used && n && strncasecmp(g_boards[i].index, idx, n) == 0) return &g_boards[i];
    return NULL;
}

static int age_of(const struct user *u) {
    int y, m, d;
    if (sscanf(u->birth, "%d-%d-%d", &y, &m, &d) != 3) return -1;
    y += y < 30 ? 2000 : 1900;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int age = tm->tm_year + 1900 - y;
    if (tm->tm_mon + 1 < m || (tm->tm_mon + 1 == m && tm->tm_mday < d)) age--;
    return age;
}

static bool board_common(const struct user *u, const struct board *b) {
    if (b->age_max > 0) {
        int age = age_of(u);
        if (age < b->age_min || age >= b->age_max) return false;
    }
    if (b->sex == 1 && strcmp(u->sex, "M") != 0) return false;
    if (b->sex == 2 && strcmp(u->sex, "F") != 0) return false;
    return true;
}

bool board_can_read(const struct user *u, const struct board *b) {
    if (!b->used) return false;
    if (u->level >= LV_SYSOP || (b->bop == u->id && u->id != 0)) return true;
    if (u->level < b->rlevel || !board_common(u, b)) return false;
    if (b->cug) {
        char r = *cug_right(u->id, b->no);
        return r == 'a' || r == 'r';
    }
    return true;
}

bool board_can_write(const struct user *u, const struct board *b) {
    if (!b->used) return false;
    if (u->level >= LV_SYSOP || (b->bop == u->id && u->id != 0)) return true;
    if (u->level < b->wlevel || !board_common(u, b)) return false;
    if (b->cug) {
        char r = *cug_right(u->id, b->no);
        return r == 'a' || r == 'w';
    }
    return true;
}

/* ------------------------------------------------------------ 既読位置・巡回・CUG */

int *read_ptr(int user, int board) { return &g_ptrs[user * MAX_BOARDS + board]; }
char *bset_flag(int user, int board) { return &g_bset[user * MAX_BOARDS + board]; }
char *cug_right(int user, int board) { return &g_cug[user * MAX_BOARDS + board]; }

void db_save_ptrs(void) {
    exec("BEGIN");
    exec("DELETE FROM pointers");
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "INSERT INTO pointers VALUES (?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        for (int u = 0; u < g_nusers; u++)
            for (int b = 0; b < MAX_BOARDS; b++) {
                int p = *read_ptr(u, b);
                char s = *bset_flag(u, b), c = *cug_right(u, b);
                if (!p && s == 'Y' && c == 'n') continue;
                char ss[2] = {s, 0}, cs[2] = {c, 0};
                sqlite3_bind_int(st, 1, u);
                sqlite3_bind_int(st, 2, b);
                sqlite3_bind_int(st, 3, p);
                sqlite3_bind_text(st, 4, ss, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 5, cs, -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
                sqlite3_reset(st);
            }
        sqlite3_finalize(st);
    }
    exec("COMMIT");
}

static void load_ptrs(void) {
    memset(g_bset, 'Y', (size_t)MAX_USERS * MAX_BOARDS);
    memset(g_cug, 'n', (size_t)MAX_USERS * MAX_BOARDS);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT user, board, ptr, bset, cug FROM pointers", -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int u = sqlite3_column_int(st, 0), b = sqlite3_column_int(st, 1);
        if (u < 0 || u >= MAX_USERS || b < 0 || b >= MAX_BOARDS) continue;
        *read_ptr(u, b) = sqlite3_column_int(st, 2);
        const unsigned char *s = sqlite3_column_text(st, 3), *c = sqlite3_column_text(st, 4);
        *bset_flag(u, b) = s && *s ? (char)*s : 'Y';
        *cug_right(u, b) = c && *c ? (char)*c : 'n';
    }
    sqlite3_finalize(st);
}

/* ------------------------------------------------------------ メッセージ (タイトルと本文) */

void db_save_msgs(void) {
    save_table("titles", msg_fields, NF(msg_fields), g_msgs, sizeof *g_msgs, g_nmsgs, NULL);
}

static void grow_msgs(void) {
    if (g_nmsgs < msgs_cap) return;
    msgs_cap = msgs_cap ? msgs_cap * 2 : 256;
    g_msgs = realloc(g_msgs, sizeof *g_msgs * (size_t)msgs_cap);
}
static void each_msg(void *r) {
    grow_msgs();
    g_msgs[g_nmsgs++] = *(struct msg *)r;
}

struct msg *msg_get(int board, int seq) {
    for (int i = 0; i < g_nmsgs; i++)
        if (g_msgs[i].board == board && g_msgs[i].seq == seq && !g_msgs[i].deleted) return &g_msgs[i];
    return NULL;
}

int msg_add(struct msg *m, const char *body) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "INSERT INTO bodies (body) VALUES (?)", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, body, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    m->off = (long)sqlite3_last_insert_rowid(db);
    m->len = (long)strlen(body);
    struct board *b = &g_boards[m->board];
    if (b->next_seq < 1) b->next_seq = 1;
    m->seq = b->next_seq++;
    m->t = time(NULL);
    grow_msgs();
    g_msgs[g_nmsgs++] = *m;
    if (m->reply_to) {
        struct msg *p = msg_get(m->board, m->reply_to);
        if (p) p->replies++;
    }
    db_save_msgs();
    db_save_boards();
    return m->seq;
}

char *msg_body(const struct msg *m) {
    sqlite3_stmt *st;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT body FROM bodies WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, m->off);
        if (sqlite3_step(st) == SQLITE_ROW) out = strdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    return out ? out : strdup("");
}

long file_add(const void *data, size_t len) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "INSERT INTO files (data) VALUES (?)", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_blob(st, 1, data, (int)len, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? (long)sqlite3_last_insert_rowid(db) : 0;
}

void *file_get(long fid, size_t *len) {
    sqlite3_stmt *st;
    void *out = NULL;
    *len = 0;
    if (sqlite3_prepare_v2(db, "SELECT data FROM files WHERE id = ?", -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(st, 1, fid);
    if (sqlite3_step(st) == SQLITE_ROW) {
        int n = sqlite3_column_bytes(st, 0);
        out = malloc((size_t)n + 1);
        if (n) memcpy(out, sqlite3_column_blob(st, 0), (size_t)n);
        *len = (size_t)n;
    }
    sqlite3_finalize(st);
    return out;
}

int board_count(int board) {
    int n = 0;
    for (int i = 0; i < g_nmsgs; i++) if (g_msgs[i].board == board && !g_msgs[i].deleted) n++;
    return n;
}

int board_unread(int user, int board) {
    int n = 0, p = *read_ptr(user, board);
    for (int i = 0; i < g_nmsgs; i++)
        if (g_msgs[i].board == board && !g_msgs[i].deleted && g_msgs[i].seq > p) n++;
    return n;
}

bool mail_is_for(const struct msg *m, int user, int *slot) {
    for (int k = 0; k < MAX_MAIL_TO; k++)
        if (m->to[k] == user && m->to_state[k] != 'd') {
            if (slot) *slot = k;
            return true;
        }
    return false;
}

/* ファイルメンテナンス: 削除済み (と親が消えたリプライ) と、保存数を超えた古いものを取り除いて番号を詰める */
int filem(void) {
    for (int b = 0; b < MAX_BOARDS; b++) {
        if (!g_boards[b].used) continue;
        int keep = g_boards[b].keep, alive = board_count(b);
        for (int i = 0; i < g_nmsgs && keep > 0 && alive > keep; i++)
            if (g_msgs[i].board == b && !g_msgs[i].deleted) {
                g_msgs[i].deleted = true;
                alive--;
            }
        for (bool changed = true; changed;) {
            changed = false;
            for (int i = 0; i < g_nmsgs; i++) {
                struct msg *m = &g_msgs[i];
                if (m->board == b && !m->deleted && m->reply_to && !msg_get(b, m->reply_to)) {
                    m->deleted = true;
                    changed = true;
                }
            }
        }
    }
    int removed = 0;
    int *newseq = calloc((size_t)g_nmsgs + 1, sizeof(int));
    int next[MAX_BOARDS];
    for (int b = 0; b < MAX_BOARDS; b++) next[b] = 1;
    for (int i = 0; i < g_nmsgs; i++)
        if (!g_msgs[i].deleted) newseq[i] = next[g_msgs[i].board]++;
    /* 既読位置を新しい番号に合わせる */
    for (int u = 0; u < g_nusers; u++)
        for (int b = 0; b < MAX_BOARDS; b++) {
            int p = *read_ptr(u, b), np = 0;
            for (int i = 0; i < g_nmsgs; i++)
                if (g_msgs[i].board == b && !g_msgs[i].deleted && g_msgs[i].seq <= p) np = newseq[i];
            *read_ptr(u, b) = np;
        }
    exec("BEGIN");
    sqlite3_stmt *del = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM bodies WHERE id = ?", -1, &del, NULL);
    sqlite3_stmt *delf = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM files WHERE id = ?", -1, &delf, NULL);
    int w = 0;
    for (int i = 0; i < g_nmsgs; i++) {
        struct msg m = g_msgs[i];
        if (m.deleted) {
            removed++;
            if (del) {
                sqlite3_bind_int64(del, 1, m.off);
                sqlite3_step(del);
                sqlite3_reset(del);
            }
            if (delf && m.fid) {
                sqlite3_bind_int64(delf, 1, m.fid);
                sqlite3_step(delf);
                sqlite3_reset(delf);
            }
            continue;
        }
        if (m.reply_to) {
            int nr = 0;
            for (int k = 0; k < g_nmsgs; k++)
                if (g_msgs[k].board == m.board && g_msgs[k].seq == m.reply_to && !g_msgs[k].deleted) nr = newseq[k];
            m.reply_to = nr;
        }
        m.seq = newseq[i];
        g_msgs[w++] = m;
    }
    if (del) sqlite3_finalize(del);
    if (delf) sqlite3_finalize(delf);
    exec("COMMIT");
    g_nmsgs = w;
    for (int b = 0; b < MAX_BOARDS; b++) if (g_boards[b].used) g_boards[b].next_seq = next[b];
    free(newseq);
    db_save_msgs();
    db_save_boards();
    db_save_ptrs();
    return removed;
}

/* ------------------------------------------------------------ ログ */

void log_add(const struct logent *e) {
    if (log_n < LOG_MAX) g_log[(log_head + log_n++) % LOG_MAX] = *e;
    else {
        g_log[log_head] = *e;
        log_head = (log_head + 1) % LOG_MAX;
    }
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "INSERT INTO log VALUES (?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        bind_fields(st, log_fields, NF(log_fields), e);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    char sql[128];
    snprintf(sql, sizeof sql, "DELETE FROM log WHERE rowid <= (SELECT max(rowid) FROM log) - %d", LOG_MAX);
    exec(sql);
}

/* 新しい順に最大 max 件 */
int log_recent(struct logent *out, int max) {
    int n = 0;
    for (int k = log_n - 1; k >= 0 && n < max; k--) out[n++] = g_log[(log_head + k) % LOG_MAX];
    return n;
}
static void each_log(void *r) {
    if (log_n < LOG_MAX) g_log[log_n++] = *(struct logent *)r;
}

/* ------------------------------------------------------------ 入会申請 */

void app_save(void) {
    save_table("newmem", app_fields, NF(app_fields), g_apps, sizeof *g_apps, apps_n, NULL);
}

static void app_push(const struct application *a) {
    if (apps_n == apps_cap) {
        apps_cap = apps_cap ? apps_cap * 2 : 16;
        g_apps = realloc(g_apps, sizeof *g_apps * (size_t)apps_cap);
    }
    g_apps[apps_n++] = *a;
}
static void each_app(void *r) { app_push(r); }

int app_add(const struct application *a) {
    struct application c = *a;
    c.no = apps_n + 1;
    app_push(&c);
    app_save();
    return apps_n;
}

struct application *app_list(int *n) {
    *n = apps_n;
    return g_apps;
}

/* ------------------------------------------------------------ システム設定 */

static void sys_put(sqlite3_stmt *st, const char *k, const char *v) {
    sqlite3_bind_text(st, 1, k, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, v, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_reset(st);
}

void db_save_sys(void) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO sys VALUES (?,?)", -1, &st, NULL) != SQLITE_OK) return;
    exec("BEGIN");
    char v[64];
    sys_put(st, "net_id", g_sys.net_id);
#define PUT(name, fmt) snprintf(v, sizeof v, fmt, g_sys.name); sys_put(st, #name, v);
    PUT(min_minutes, "%d") PUT(user_minutes, "%d") PUT(user_level, "%d") PUT(temp_level, "%d")
    PUT(chat_level, "%d") PUT(mail_size, "%d") PUT(signup, "%d") PUT(manager, "%d") PUT(aoff, "%d")
    PUT(total_logins, "%ld") PUT(guest_logins, "%ld")
#undef PUT
    for (int i = 0; i < 10; i++) {
        char k[16];
        snprintf(k, sizeof k, "sysmes%d", i);
        sys_put(st, k, g_sys.sysmes[i]);
    }
    exec("COMMIT");
    sqlite3_finalize(st);
}

static void load_sys(void) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT key, value FROM sys", -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(st, 0), *v = (const char *)sqlite3_column_text(st, 1);
        if (!k || !v) continue;
        if (!strcmp(k, "net_id")) snprintf(g_sys.net_id, sizeof g_sys.net_id, "%s", v);
#define GET(name) else if (!strcmp(k, #name)) g_sys.name = atol(v);
        GET(min_minutes) GET(user_minutes) GET(user_level) GET(temp_level) GET(chat_level) GET(mail_size)
        GET(signup) GET(manager) GET(aoff) GET(total_logins) GET(guest_logins)
#undef GET
        else if (!strncmp(k, "sysmes", 6)) {
            int i = atoi(k + 6);
            if (i >= 0 && i < 10) snprintf(g_sys.sysmes[i], sizeof g_sys.sysmes[i], "%s", v);
        }
    }
    sqlite3_finalize(st);
}

/* ------------------------------------------------------------ 初期化 */

int db_open(void) {
    g_users = calloc(MAX_USERS, sizeof *g_users);
    g_ptrs = calloc((size_t)MAX_USERS * MAX_BOARDS, sizeof(int));
    g_bset = malloc((size_t)MAX_USERS * MAX_BOARDS);
    g_cug = malloc((size_t)MAX_USERS * MAX_BOARDS);
    g_log = calloc(LOG_MAX, sizeof *g_log);
    if (!g_users || !g_ptrs || !g_bset || !g_cug || !g_log) return -1;
    mkdir(g_cfg.data_dir, 0755);
    char path[600];
    snprintf(path, sizeof path, "%s/net-cock.db", g_cfg.data_dir);
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    exec("PRAGMA journal_mode = WAL");
    exec("PRAGMA synchronous = NORMAL");
    create_table("users", user_fields, NF(user_fields), ", PRIMARY KEY (id)");
    create_table("boards", board_fields, NF(board_fields), ", PRIMARY KEY (no)");
    create_table("titles", msg_fields, NF(msg_fields), NULL);
    exec("CREATE TABLE IF NOT EXISTS bodies (id INTEGER PRIMARY KEY, body TEXT)");
    exec("CREATE TABLE IF NOT EXISTS files (id INTEGER PRIMARY KEY, data BLOB)");
    exec("CREATE TABLE IF NOT EXISTS pointers (user INTEGER, board INTEGER, ptr INTEGER, bset TEXT, cug TEXT, "
         "PRIMARY KEY (user, board))");
    create_table("log", log_fields, NF(log_fields), NULL);
    create_table("newmem", app_fields, NF(app_fields), NULL);
    exec("CREATE TABLE IF NOT EXISTS sys (key TEXT PRIMARY KEY, value TEXT)");

    /* 既定値。ネットワーク ID の初期値は NET-COCK の配布データと同じ TEST */
    snprintf(g_sys.net_id, sizeof g_sys.net_id, "%s", g_cfg.net_id[0] ? g_cfg.net_id : "TEST");
    g_sys.min_minutes = g_cfg.guest_minutes;
    g_sys.user_minutes = g_cfg.session_minutes;
    g_sys.user_level = 50;
    g_sys.temp_level = 30;
    g_sys.chat_level = 0;
    g_sys.mail_size = 8192;
    g_sys.signup = SIGNUP_AUTO;
    g_sys.manager = 1;

    load_sys();
    load_table("users", user_fields, NF(user_fields), sizeof(struct user), "ORDER BY id", each_user);
    load_table("boards", board_fields, NF(board_fields), sizeof(struct board), NULL, each_board);
    load_ptrs();
    load_table("titles", msg_fields, NF(msg_fields), sizeof(struct msg), "ORDER BY board, seq", each_msg);
    load_table("log", log_fields, NF(log_fields), sizeof(struct logent), "ORDER BY rowid DESC LIMIT 512", each_log);
    /* 新しい順に読んだので古い順に並べ直す */
    for (int i = 0; i < log_n / 2; i++) {
        struct logent t = g_log[i];
        g_log[i] = g_log[log_n - 1 - i];
        g_log[log_n - 1 - i] = t;
    }
    load_table("newmem", app_fields, NF(app_fields), sizeof(struct application), "ORDER BY no", each_app);
    g_sys.started = time(NULL);

    /* 0 番はメール (一覧には出さない) */
    if (!g_boards[MAIL_BOARD].used) {
        struct board *m = &g_boards[MAIL_BOARD];
        m->used = true;
        m->no = MAIL_BOARD;
        m->type = BT_MAIL;
        snprintf(m->index, sizeof m->index, "MAIL");
        snprintf(m->title, sizeof m->title, "メール");
        db_save_boards();
    }
    /* 次に振る番号は、残っているメッセージの最大値より大きくする */
    for (int i = 0; i < g_nmsgs; i++) {
        struct board *b = &g_boards[g_msgs[i].board];
        if (b->next_seq <= g_msgs[i].seq) b->next_seq = g_msgs[i].seq + 1;
    }
    for (int i = 0; i < MAX_BOARDS; i++)
        if (g_boards[i].next_seq < 1) g_boards[i].next_seq = 1;

    /* 新しく始めたとき: NET-COCK と同じく、ゲスト (ID 0) と SYSOP (ID 1、パスワード ABC) だけを作る。
       ボードは作らない (SYSOP が BMAKE で作る。0 番はメール) */
    if (g_nusers == 0) {
        memset(g_bset, 'Y', (size_t)MAX_USERS * MAX_BOARDS);
        struct user *g = &g_users[0];
        memset(g, 0, sizeof *g);
        g->id = 0;
        g->flags = UF_GUEST;
        g->level = LV_GUEST;
        snprintf(g->logname, sizeof g->logname, "Guest");
        g->bs_one_col = true;
        struct user *s = &g_users[1];
        memset(s, 0, sizeof *s);
        s->id = 1;
        s->level = 255;
        s->day_minutes = s->today_left = 1440;
        snprintf(s->logname, sizeof s->logname, "Sysop");
        snprintf(s->pass, sizeof s->pass, "ABC");
        s->issued = time(NULL);
        s->bs_one_col = true;
        s->chat_on_login = true;
        g_nusers = 2;
        db_save_users();
        db_save_ptrs();
    }
    db_save_sys();
    return 0;
}

void db_save_all(void) {
    db_save_sys();
    db_save_users();
    db_save_boards();
    db_save_msgs();
    db_save_ptrs();
    app_save();
}
