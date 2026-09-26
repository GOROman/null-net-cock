/*
 * ヘルプ・テンプレート (HELP.TXT)。ブロックの番号は NET-COCK v3.60 と同じ
 */
#ifndef HELP_H
#define HELP_H

#include "session.h"

enum {
    HB_BOARD = 0,       /* ボード名の見出し */
    HB_REPLY = 1,       /* リプライ先 */
    HB_HEADER = 2,      /* メッセージのヘッダ */
    HB_GUEST_OPEN = 3,  /* ゲストのオープニング */
    HB_USER_OPEN = 4,   /* 会員のオープニング */
    HB_LOGOUT = 5,
    HB_UREAD = 6,
    HB_USTAT = 7,
    HB_ACCESS = 8,
    HB_CHAT_HELP = 9,
    HB_WRITE_HELP = 10,
    HB_READ_HELP = 11,
    HB_BATCH_HELP = 12,
    HB_AUTO_SIGNUP = 13,
    HB_MANUAL_SIGNUP = 14,
};

struct help_ctx {
    struct sess *s;
    int board;                  /* -1 は無し */
    const struct msg *m;        /* 今のメッセージ */
    const struct user *target;  /* UREAD の相手 (/m) */
    int ptr, total;             /* ヘッダの (位置/総数) */
    const char *speed;          /* 接続の速度 */
    time_t login_at;
    const char *mail_notice;    /* /> で出す新着メールの案内 */
    const char *sched;          /* /| で出すスケジュールのメッセージ */
};

int help_load(const char *path);               /* 読めたブロックの数 */
const char *help_block(int id);                /* NULL はテンプレートが無い (ACCESS の既定など) */
bool help_has(int id);
char *help_expand(int id, const struct help_ctx *c); /* g_lock を持って呼ぶ。malloc した文字列 */
int help_show(struct sess *s, int id, const struct help_ctx *c);

#endif
