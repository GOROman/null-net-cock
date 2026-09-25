/*
 * セッション (1 回線ぶんの利用者の状態) とコマンドの共通部品
 */
#ifndef SESSION_H
#define SESSION_H

#include "db.h"
#include "msg.h"
#include "nc.h"

struct sess {
    struct term *t;
    int no;                 /* 回線番号 */
    int uid;                /* ログイン中の ID (0 = ゲスト) */
    time_t login_at;
    char peer[64];
    char speed[32];
    /* 直前に読んだメッセージ (F でリプライするため) */
    int last_board, last_seq;
    /* 書きかけの文章 (Q で抜けたとき残す) */
    char *draft_title, *draft_body;
    int draft_board;
    /* 受信したが書き込んでいないファイル (次の書き込みで使える) */
    unsigned char *upload;
    size_t upload_len;
    char upload_name[16];
    /* BATCH のリスト */
    struct { int board, seq; } batch[16];
    int nbatch;
    bool nonstop;           /* RALL の途中で V を押した */
};

#define MAX_BATCH 16

#define USER(s) (&g_users[(s)->uid])
#define IS_GUEST(s) ((s)->uid == 0)
#define IS_SYSOP(s) (USER(s)->level >= LV_SYSOP)
#define IS_MANAGER(s) ((s)->uid == g_sys.manager && IS_SYSOP(s))

/* 失敗 (切断など) ならそのまま呼び出し元に返す */
#define CHK(expr) do { int _r = (expr); if (_r < 0) return _r; } while (0)

/* 文字列を出す / メッセージ ID の文言を出して改行する / 文言だけ (改行なし) */
int out(struct sess *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int outm(struct sess *s, int id);
int outm_nl(struct sess *s, int id);
/* 1 行入力 (プロンプトはメッセージ ID) */
int ask(struct sess *s, int id, char *buf, size_t sz, int flags);
int ask_str(struct sess *s, const char *prompt, char *buf, size_t sz, int flags);
/* (y/n) の問い。1: Y 0: N (空 Enter も N) 負: エラー */
int yn(struct sess *s, int id);
/* 数値の入力。空なら def */
int ask_int(struct sess *s, int id, int def, int *val);
/* ボードを選ぶ (? で一覧)。選んだボード番号、やめたら -1、負のエラーは < -1 で返す */
int choose_board(struct sess *s, int prompt_id, bool for_write);
/* 会員を ID かログネームで選ぶ */
struct user *choose_user(struct sess *s, int prompt_id);
/* 一覧の表示中に「続けますか」を聞く。counter を渡す。1: やめる */
int paging(struct sess *s, int *count, bool continuous);

/* ボード・メール (cmd_board.c) */
int cmd_bread(struct sess *s, const char *arg);
int cmd_bwrite(struct sess *s, const char *arg);
int cmd_rall(struct sess *s, const char *arg);
int cmd_rnall(struct sess *s, const char *arg);
int cmd_bset(struct sess *s, const char *arg);
int cmd_pmove(struct sess *s, const char *arg);
int cmd_mread(struct sess *s, const char *arg);
int cmd_mwrite(struct sess *s, const char *arg);
int cmd_mcheck(struct sess *s, const char *arg);
int cmd_mbset(struct sess *s, const char *arg);
int cmd_batch(struct sess *s, const char *arg);
int cmd_bchange(struct sess *s, const char *arg);
enum { RB_NORMAL, RB_RALL, RB_NONSTOP };
int read_board(struct sess *s, int board, int mode);
int login_mail_notice(struct sess *s);
void print_board_list(struct sess *s, bool for_write);

/* 利用者・チャット・入会・SYSOP (cmd_misc.c) */
int cmd_log(struct sess *s, const char *arg);
int cmd_uread(struct sess *s, const char *arg);
int cmd_mode(struct sess *s, const char *arg);
int cmd_uwrite(struct sess *s, const char *arg);
int cmd_pass(struct sess *s, const char *arg);
int cmd_idlist(struct sess *s, const char *arg);
int cmd_cls(struct sess *s, const char *arg);
int cmd_ustat(struct sess *s, const char *arg);
int cmd_version(struct sess *s, const char *arg);
int cmd_chat(struct sess *s, const char *arg);
int cmd_llist(struct sess *s, const char *arg);
int cmd_coff(struct sess *s, const char *arg);
int cmd_con(struct sess *s, const char *arg);
int cmd_join(struct sess *s, const char *arg);
int cmd_newmem(struct sess *s, const char *arg);
int cmd_makeid(struct sess *s, const char *arg);
int cmd_gulv(struct sess *s, const char *arg);
int cmd_ulevel(struct sess *s, const char *arg);
int cmd_utime(struct sess *s, const char *arg);
int cmd_mchange(struct sess *s, const char *arg);
int cmd_idkill(struct sess *s, const char *arg);
int cmd_idset(struct sess *s, const char *arg);
int cmd_upass(struct sess *s, const char *arg);
int cmd_bmake(struct sess *s, const char *arg);
int cmd_bkill(struct sess *s, const char *arg);
int cmd_btitle(struct sess *s, const char *arg);
int cmd_buser(struct sess *s, const char *arg);
int cmd_mesedit(struct sess *s, const char *arg);
int cmd_store(struct sess *s, const char *arg);
int cmd_secret(struct sess *s, const char *arg);
int cmd_secoff(struct sess *s, const char *arg);
int cmd_sysset(struct sess *s, const char *arg);
int cmd_signup(struct sess *s, const char *arg);
int cmd_aon(struct sess *s, const char *arg);
int cmd_aoff(struct sess *s, const char *arg);
int cmd_filem(struct sess *s, const char *arg);
int cmd_report(struct sess *s, const char *arg);
int user_setup(struct sess *s, bool initial);
int bs_test(struct sess *s);
int application_input(struct sess *s, struct application *a);
int signup_auto(struct sess *s, bool ask_first);

#endif
