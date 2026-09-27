/*
 * 会員管理: MAKEID / GULV / ULEVEL / UTIME / IDKILL / IDCOPY / IDSET / UPASS / UDLIST / UDEDIT
 *
 * UDLIST と UDEDIT の項目番号 (0〜26) は NET-COCK と同じ。
 */
#include "session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 「〜しませんね (y/n)」形式。N のときだけ 1 */
static int yn_neg(struct sess *s, int id) {
    char a[16];
    CHK(ask(s, id, a, sizeof a, RL_UPPER));
    return a[0] == 'N';
}

/* ------------------------------------------------------------ 項目番号 */

#define NUD 28
static const char *const ud_label[NUD] = {
    "レベル", "持ち時間", "本日の残り", "メールレベル", "ログネーム", "性別", "フリガナ", "氏名", "電話番号",
    "郵便番号", "登録住所", "公開住所", "職業", "機種", "パスワード", "生年月日", "申請日", "登録日", "最終接続日",
    "書込(B)", "書込(M)", "書込(P)", "ログイン数", "総時間", "今月", "前月", "ID", "X",
};

static void day(time_t t, char *buf, size_t sz) {
    if (!t) snprintf(buf, sz, "-");
    else strftime(buf, sz, "%Y-%m-%d", localtime(&t));
}

static void ud_get(const struct user *u, int k, char *buf, size_t sz) {
    switch (k) {
    case 0: snprintf(buf, sz, "%d", u->level); break;
    case 1: snprintf(buf, sz, "%d", u->day_minutes); break;
    case 2: snprintf(buf, sz, "%d", u->today_left); break;
    case 3: snprintf(buf, sz, "%d", u->mail_level); break;
    case 4: snprintf(buf, sz, "%s", u->logname); break;
    case 5: snprintf(buf, sz, "%s", u->sex); break;
    case 6: snprintf(buf, sz, "%s", u->kana); break;
    case 7: snprintf(buf, sz, "%s", u->name); break;
    case 8: snprintf(buf, sz, "%s", u->tel); break;
    case 9: snprintf(buf, sz, "%s", u->zip); break;
    case 10: snprintf(buf, sz, "%s", u->addr_priv); break;
    case 11: snprintf(buf, sz, "%s", u->addr_pub); break;
    case 12: snprintf(buf, sz, "%s", u->job); break;
    case 13: snprintf(buf, sz, "%s", u->machine); break;
    case 14: snprintf(buf, sz, "%s", u->pass); break;
    case 15: snprintf(buf, sz, "%s", u->birth); break;
    case 16: day(u->applied, buf, sz); break;
    case 17: day(u->issued, buf, sz); break;
    case 18: day(u->last_login, buf, sz); break;
    case 19: snprintf(buf, sz, "%d", u->wb); break;
    case 20: snprintf(buf, sz, "%d", u->wm); break;
    case 21: snprintf(buf, sz, "%d", u->wp); break;
    case 22: snprintf(buf, sz, "%d", u->logins); break;
    case 23: snprintf(buf, sz, "%ld", u->total_sec / 60); break;
    case 24: snprintf(buf, sz, "%ld", u->month_sec / 60); break;
    case 25: snprintf(buf, sz, "%ld", u->prev_month_sec / 60); break;
    case 26: snprintf(buf, sz, "%d", u->id); break;
    case 27: snprintf(buf, sz, "%s", u->x_account); break;
    default: buf[0] = 0;
    }
}

/* 書き換えられる項目なら書き換えて true (日付・統計・ID は書き換えない) */
static bool ud_set(struct user *u, int k, const char *v) {
    int n = atoi(v);
#define S(f) snprintf(u->f, sizeof u->f, "%s", v)
    switch (k) {
    case 0: u->level = n; return true;
    case 1: u->day_minutes = n; return true;
    case 2: u->today_left = n; return true;
    case 3: u->mail_level = n; return true;
    case 4: S(logname); return true;
    case 5: S(sex); return true;
    case 6: S(kana); return true;
    case 7: S(name); return true;
    case 8: S(tel); return true;
    case 9: S(zip); return true;
    case 10: S(addr_priv); return true;
    case 11: S(addr_pub); return true;
    case 12: S(job); return true;
    case 13: S(machine); return true;
    case 14: S(pass); return true;
    case 15: S(birth); return true;
    case 27: S(x_account); return true;
    }
#undef S
    return false;
}

/* 項目の並び。a/b/c は原典と同じ組み合わせ */
static int ask_fields(struct sess *s, int *fields) {
    static const int set_a[] = {4, 6, 7, 8, 9, 10, 16}, set_b[] = {0, 1, 3, 5, 18, 19, 20, 21, 22, 23, 24, 25},
                     set_c[] = {4, 5, 11, 12, 13, 15};
    char a[128];
    CHK(ask_str(s, "出力する項目 (a/b/c/u)＞", a, sizeof a, RL_UPPER));
    const int *src = NULL;
    int n = 0;
    if (a[0] == 'A') src = set_a, n = 7;
    else if (a[0] == 'B') src = set_b, n = 12;
    else if (a[0] == 'C') src = set_c, n = 6;
    else if (a[0] == 'U') {
        CHK(ask_str(s, "項目の番号 (0〜27 をカンマで区切る)＞", a, sizeof a, 0));
        for (char *save, *p = strtok_r(a, ", ", &save); p && n < NUD; p = strtok_r(NULL, ", ", &save)) {
            int k = atoi(p);
            if (k >= 0 && k < NUD) fields[n++] = k;
        }
        return n;
    } else return 0;
    memcpy(fields, src, sizeof(int) * (size_t)n);
    return n;
}

static void csv_line(const struct user *u, const int *fields, int nf, char *buf, size_t sz) {
    size_t l = (size_t)snprintf(buf, sz, "%04d", u->id);
    for (int i = 0; i < nf && l < sz; i++) {
        char v[160];
        ud_get(u, fields[i], v, sizeof v);
        l += (size_t)snprintf(buf + l, sz - l, ",%s", v);
    }
    if (l < sz - 1) buf[l++] = '\n', buf[l] = 0;
}

static int csv_header(struct sess *s, const int *fields, int nf) {
    char buf[1024];
    size_t l = (size_t)snprintf(buf, sizeof buf, "ID");
    for (int i = 0; i < nf; i++) l += (size_t)snprintf(buf + l, sizeof buf - l, ",%d:%s", fields[i], ud_label[fields[i]]);
    return out(s, "%s\n", buf);
}

/* ------------------------------------------------------------ UDLIST / UDEDIT */

int cmd_udlist(struct sess *s, const char *arg) {
    int fields[NUD];
    int nf = ask_fields(s, fields);
    if (nf <= 0) return nf;
    int from;
    CHK(ask_int(s, 125, 0, &from));
    int cont = yn(s, 130);
    if (cont < 0) return cont;
    CHK(csv_header(s, fields, nf));
    int count = 0;
    for (int i = from < 0 ? 0 : from;; i++) {
        char line[2048] = "";
        pthread_mutex_lock(&g_lock);
        bool end = i >= g_nusers;
        const struct user *u = end ? NULL : user_get(i);
        if (u) csv_line(u, fields, nf, line, sizeof line);
        pthread_mutex_unlock(&g_lock);
        if (end) break;
        if (!line[0]) continue;
        CHK(out(s, "%s", line));
        int r = paging(s, &count, cont);
        if (r) return r < 0 ? r : 0;
    }
    return 0;
}

struct cond {
    int field, op;
    char value[80];
};

static bool match(const struct user *u, const struct cond *c, int nc) {
    for (int i = 0; i < nc; i++) {
        char v[160];
        ud_get(u, c[i].field, v, sizeof v);
        char *e1, *e2;
        long a = strtol(v, &e1, 10), b = strtol(c[i].value, &e2, 10);
        bool num = *v && *c[i].value && !*e1 && !*e2;
        int cmp = num ? (a > b) - (a < b) : strcmp(v, c[i].value);
        bool ok = c[i].op == 0 ? cmp == 0 : c[i].op == 1 ? cmp != 0 : c[i].op == 2 ? cmp >= 0 : cmp <= 0;
        if (!ok) return false;
    }
    return true;
}

static int ask_cond(struct sess *s, struct cond *c) {
    char a[80];
    CHK(ask_str(s, "条件の項目の番号＞", a, sizeof a, 0));
    if (!a[0]) return 0;
    c->field = atoi(a);
    if (c->field < 0 || c->field >= NUD) return 0;
    CHK(ask_str(s, "値＞", c->value, sizeof c->value, 0));
    CHK(ask_str(s, "条件 (0:一致 1:不一致 2:以上 3:以下)＞", a, sizeof a, 0));
    c->op = atoi(a);
    return c->op >= 0 && c->op <= 3;
}

int cmd_udedit(struct sess *s, const char *arg) {
    int fields[NUD];
    int nf = ask_fields(s, fields);
    if (nf <= 0) return nf;
    struct cond conds[8];
    int nc = 0;
    int r = ask_cond(s, &conds[0]);
    if (r < 0) return r;
    nc = r;
    for (;;) {
        char a[8];
        CHK(ask_str(s, "y:検索する a:条件を足す s:1 件ずつ q:やめる＞", a, sizeof a, RL_UPPER));
        if (a[0] == 'Q' || !a[0]) return 0;
        if (a[0] == 'A') {
            if (nc < 8 && (r = ask_cond(s, &conds[nc])) > 0) nc++;
            if (r < 0) return r;
            continue;
        }
        if (a[0] != 'Y' && a[0] != 'S') continue;
        bool step = a[0] == 'S';
        CHK(csv_header(s, fields, nf));
        int count = 0;
        for (int i = 0;; i++) {
            char line[2048] = "";
            pthread_mutex_lock(&g_lock);
            bool end = i >= g_nusers;
            struct user *u = end ? NULL : user_get(i);
            if (u && match(u, conds, nc)) csv_line(u, fields, nf, line, sizeof line);
            pthread_mutex_unlock(&g_lock);
            if (end) break;
            if (!line[0]) continue;
            CHK(out(s, "%s", line));
            if (!step) {
                if ((r = paging(s, &count, false))) return r < 0 ? r : 0;
                continue;
            }
            for (bool next = false; !next;) {
                char c[8];
                CHK(ask_str(s, "n:次 e:書き換える d:削除 q:終わる＞", c, sizeof c, RL_UPPER));
                if (c[0] == 'Q') return 0;
                if (c[0] == 'N' || !c[0]) next = true;
                else if (c[0] == 'E') {
                    char k[8], v[128];
                    CHK(ask_str(s, "項目の番号＞", k, sizeof k, 0));
                    CHK(ask_str(s, "新しい値＞", v, sizeof v, 0));
                    pthread_mutex_lock(&g_lock);
                    bool ok = k[0] && ud_set(u, atoi(k), v);
                    if (ok) db_save_users();
                    csv_line(u, fields, nf, line, sizeof line);
                    pthread_mutex_unlock(&g_lock);
                    CHK(ok ? out(s, "%s", line) : out(s, "==== その項目は書き換えられません ====\n"));
                } else if (c[0] == 'D') {
                    pthread_mutex_lock(&g_lock);
                    bool ok = u->id > 1 && u->id != g_sys.manager;
                    if (ok) u->flags |= UF_DELETED;
                    db_save_users();
                    pthread_mutex_unlock(&g_lock);
                    CHK(ok ? outm_nl(s, 204) : out(s, "==== この ID は削除できません ====\n"));
                    next = true;
                }
            }
        }
    }
}

/* ------------------------------------------------------------ フリガナで探す */

static int kana_search(struct sess *s) {
    char key[64];
    CHK(ask(s, 357, key, sizeof key, 0));
    if (!key[0]) return 0;
    for (int i = 0;; i++) {
        char line[512] = "";
        pthread_mutex_lock(&g_lock);
        bool end = i >= g_nusers;
        const struct user *u = end ? NULL : user_get(i);
        if (u && !strncmp(u->kana, key, strlen(key)))
            snprintf(line, sizeof line, "%04d,%s,%s,%s,%s,%s,%d\n", u->id, u->logname, u->kana, u->name, u->addr_priv,
                     u->tel, u->level);
        pthread_mutex_unlock(&g_lock);
        if (end) break;
        if (line[0]) CHK(out(s, "%s", line));
    }
    return 0;
}

/* ------------------------------------------------------------ MAKEID / GULV */

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
        CHK(out(s, "\n#%d %s  ハンドル名 %s%s%s\n氏名 %s (%s)\n〒%s %s\nTEL %s\n", a.no, ts, a.logname,
                a.x_account[0] ? "  X @" : "", a.x_account, a.name, a.kana, a.zip, a.addr, a.tel));
        char c[16];
        CHK(ask(s, 31, c, sizeof c, RL_UPPER));
        if (c[0] == 'Q') return outm_nl(s, 30);
        if (c[0] == 'S') {
            CHK(kana_search(s));
            continue;
        }
        if (c[0] != 'Y' && c[0] != 'O' && c[0] != 'N') continue;
        pthread_mutex_lock(&g_lock);
        int id = 0;
        if (c[0] != 'N') {
            /* o: 削除した ID の番号を使い回す (無ければ新しい番号) */
            int at = 0;
            if (c[0] == 'O')
                for (int i = 2; i < g_nusers && !at; i++)
                    if (g_users[i].id == i && (g_users[i].flags & UF_DELETED)) at = i;
            id = issue_from(&a, g_sys.user_level, at);
        }
        list = app_list(&n);
        list[idx].issued_id = id > 0 ? id : -1;
        app_save();
        pthread_mutex_unlock(&g_lock);
        if (c[0] != 'N' && id <= 0) return outm_nl(s, 34);
        if (id > 0) {
            nc_log("CH%02d: MAKEID で ID %d を発行", s->no, id);
            CHK(out(s, "%s%04d %s\n", g_sys.net_id, id, M(32)));
        } else CHK(outm_nl(s, 33));
    }
}

/* 仮会員を 1 人ずつ: y 一般にする / n そのまま (次回も対象) / d 削除 / s フリガナで探す / q やめる */
int cmd_gulv(struct sess *s, const char *arg) {
    for (int i = 2;; i++) {
        pthread_mutex_lock(&g_lock);
        bool end = i >= g_nusers;
        struct user *u = end ? NULL : user_get(i);
        bool target = u && u->level > 0 && u->level < g_sys.user_level;
        pthread_mutex_unlock(&g_lock);
        if (end) break;
        if (!target) continue;
        CHK(show_user(s, u));
        for (;;) {
            char c[16];
            CHK(ask(s, 358, c, sizeof c, RL_UPPER));
            if (c[0] == 'Q') return 0;
            if (c[0] == 'S') {
                CHK(kana_search(s));
                continue;
            }
            pthread_mutex_lock(&g_lock);
            if (c[0] == 'Y') u->level = g_sys.user_level;
            if (c[0] == 'D') u->flags |= UF_DELETED;
            db_save_users();
            pthread_mutex_unlock(&g_lock);
            if (c[0] == 'Y') CHK(outm_nl(s, 359));
            if (c[0] == 'D') CHK(outm_nl(s, 204));
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ ULEVEL / UTIME */

static int set_lt(struct sess *s, bool level) {
    struct user *u = choose_user(s, 216);
    if (!u) return 0;
    pthread_mutex_lock(&g_lock);
    int lv = u->level, tm = u->day_minutes;
    pthread_mutex_unlock(&g_lock);
    CHK(out(s, "%04d:%s  L:%d T:%d\n", u->id, u->logname, lv, tm));
    int v;
    CHK(ask_int(s, level ? 153 : 154, level ? lv : tm, &v));
    if (v < 0 || v > (level ? 255 : 1440)) return 0;
    pthread_mutex_lock(&g_lock);
    if (level) u->level = v;
    else u->day_minutes = u->today_left = v;
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 220);
}

int cmd_ulevel(struct sess *s, const char *arg) { return set_lt(s, true); }
int cmd_utime(struct sess *s, const char *arg) { return set_lt(s, false); }

/* ------------------------------------------------------------ IDKILL / IDCOPY / IDSET / UPASS */

/* 会員管理者・ゲスト・初期シスオペ・自分は対象にできない */
static bool protected_id(struct sess *s, int id) {
    pthread_mutex_lock(&g_lock);
    bool p = id <= 1 || id == g_sys.manager || id == s->uid;
    pthread_mutex_unlock(&g_lock);
    return p;
}

/* ID を削除状態にし、その人の書き込みとログを消す */
int cmd_idkill(struct sess *s, const char *arg) {
    struct user *u = choose_user(s, 205);
    if (!u) return 0;
    if (protected_id(s, u->id)) return out(s, "==== この ID は削除できません ====\n");
    CHK(out(s, "%04d:%s\n", u->id, u->logname));
    int r = yn_neg(s, 203);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    u->flags |= UF_DELETED;
    int n = 0;
    for (int i = 0; i < g_nmsgs; i++)
        if (g_msgs[i].from == u->id && !g_msgs[i].deleted) g_msgs[i].deleted = true, n++;
    log_remove_user(u->id);
    db_save_users();
    db_save_msgs();
    pthread_mutex_unlock(&g_lock);
    nc_log("CH%02d: ID %d を削除 (書き込み %d 件)", s->no, u->id, n);
    CHK(outm_nl(s, 204));
    return n ? outm_nl(s, 360) : 0;
}

/* 会員データを丸ごと写す。書き込みとログの ID もコピー先に書き換える */
int cmd_idcopy(struct sess *s, const char *arg) {
    struct user *from = choose_user(s, 206);
    if (!from) return 0;
    int to;
    CHK(ask_int(s, 207, -1, &to));
    if (to < 2 || to >= MAX_USERS || to == from->id || protected_id(s, to) || protected_id(s, from->id))
        return out(s, "==== その ID には複写できません ====\n");
    CHK(out(s, "%04d → %04d\n", from->id, to));
    int r = yn_neg(s, 208);
    if (r <= 0) return r;
    pthread_mutex_lock(&g_lock);
    struct user copy = *from;
    copy.id = to;
    g_users[to] = copy;
    if (to + 1 > g_nusers) g_nusers = to + 1;
    for (int b = 0; b < MAX_BOARDS; b++) {
        *read_ptr(to, b) = *read_ptr(from->id, b);
        *bset_flag(to, b) = *bset_flag(from->id, b);
        *cug_right(to, b) = *cug_right(from->id, b);
    }
    for (int i = 0; i < g_nmsgs; i++) {
        if (g_msgs[i].from == from->id) g_msgs[i].from = to;
        for (int k = 0; k < MAX_MAIL_TO; k++)
            if (g_msgs[i].to[k] == from->id) g_msgs[i].to[k] = to;
    }
    db_save_users();
    db_save_msgs();
    db_save_ptrs();
    pthread_mutex_unlock(&g_lock);
    nc_log("CH%02d: ID %d を %d に複写", s->no, from->id, to);
    return outm_nl(s, 209);
}

int cmd_idset(struct sess *s, const char *arg) {
    char a[64];
    CHK(ask(s, 216, a, sizeof a, 0));
    if (!a[0]) return 0;
    int id = atoi(a);
    if (id <= 1 || id >= MAX_USERS) return outm_nl(s, 135);
    if (protected_id(s, id)) return out(s, "==== この ID は変えられません ====\n");
    static const struct { int msg, flag; } q[] = {{227, UF_GUEST}, {228, UF_DELETED}, {229, UF_NEW}};
    pthread_mutex_lock(&g_lock);
    struct user *u = &g_users[id];
    bool exists = id < g_nusers && u->id == id;
    int flags = u->flags;
    pthread_mutex_unlock(&g_lock);
    if (!exists) return outm_nl(s, 135);
    for (size_t i = 0; i < sizeof q / sizeof q[0]; i++) {
        char c[16];
        CHK(out(s, "[%c] ", flags & q[i].flag ? 'N' : 'Y'));
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

/* パスワードを表示し、変えるならログネームとパスワードを入れ直す */
int cmd_upass(struct sess *s, const char *arg) {
    struct user *u = choose_user(s, 247);
    if (!u) return 0;
    if (u->id == g_sys.manager && u->id != s->uid) return out(s, "==== 会員管理者は対象にできません ====\n");
    pthread_mutex_lock(&g_lock);
    char name[64], pass[32];
    snprintf(name, sizeof name, "%s", u->logname);
    snprintf(pass, sizeof pass, "%s", u->pass);
    pthread_mutex_unlock(&g_lock);
    CHK(out(s, "%s%s\n%s%s\n", M(250), name, M(248), pass));
    int r = yn_neg(s, 249);
    if (r <= 0) return r;
    char nn[64], np[32];
    CHK(ask(s, 9, nn, sizeof nn, 0));
    CHK(ask(s, 24, np, sizeof np, 0));
    pthread_mutex_lock(&g_lock);
    if (nn[0]) snprintf(u->logname, sizeof u->logname, "%s", nn);
    if (np[0]) snprintf(u->pass, sizeof u->pass, "%s", np);
    db_save_users();
    pthread_mutex_unlock(&g_lock);
    return outm_nl(s, 133);
}
