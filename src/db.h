/*
 * データ: 会員・ボード・メッセージ・既読位置・ログ・入会申請・システム設定
 *
 * すべてメモリに載せ、変更のたびに SQLite (data_dir/net-cock.db) へ書き出す。
 * 呼び出し側は g_lock を持った状態で読み書きすること。
 */
#ifndef DB_H
#define DB_H

#include "nc.h"

#define MAX_USERS 2048
#define MAX_BOARDS 64
#define MAIL_BOARD 0
#define MAX_MAIL_TO 4
#define MAX_TITLE_BYTES 200   /* 全角 50 文字ぶん */
#define MAX_BODY_BYTES 16384

/* 会員の状態フラグ */
enum { UF_GUEST = 1, UF_DELETED = 2, UF_NEW = 4 };

#define LV_GUEST 0
#define LV_SYSOP 100
#define LV_SIGOP 80

struct user {
    int id;
    int flags;
    int level;
    int day_minutes;        /* 1 日の持ち時間 */
    int today_left;         /* 本日の残り (分) */
    int mail_level;
    char logname[64];
    char sex[8];            /* M / F / 空 */
    char kana[64];
    char name[64];
    char tel[32];
    char zip[16];
    char addr_priv[128];
    char addr_pub[128];
    char job[64];
    char machine[48];
    char pass[32];
    char birth[16];         /* YY-MM-DD */
    char intro[128];
    time_t applied, issued, last_login, prev_login;
    int wb, wm, wp;         /* 書き込み数 (ボード / メール / プログラム) */
    int logins, logins_month, logins_prev;
    long total_sec, month_sec, prev_month_sec, today_sec;
    int pass_miss;
    bool esc, bs_one_col, menu, chat_on_login, mailbox_closed, secret;
    bool menu_always;       /* メニュー方式で毎回メニューを出す */
    /* チャットの設定 (.X): ESC で色を付ける / 入力をエコーしない / 自分の発言も表示する / 発言者を ID で出す
       (古いデータに列を足したとき 0 になるので、0 が普通の動きになる向きにしてある) */
    bool chat_esc, chat_noecho, chat_self, chat_byid;
    int lcall;              /* LCSET: 0 なし / 1 次のシステムダウンまで / 2 解除するまで */
    char x_account[32];     /* X のアカウント (@ なし。空は未登録) */
};

enum board_type { BT_BOARD = 'B', BT_MAIL = 'M', BT_PROGRAM = 'P' };

struct board {
    bool used;
    int no;
    int bop;                /* ボードオペの ID */
    int rlevel, wlevel;
    int group, age_min, age_max, keep, sex;
    char type;
    bool esc, cug, timelimit;
    char index[16];
    char title[128];
    int next_seq;
};

struct msg {
    int board;
    int seq;
    time_t t;
    char title[MAX_TITLE_BYTES + 1];
    int from;
    int reply_to;           /* リプライ先の番号 (0 は無し) */
    int replies;
    long off, len;          /* 本文 (bodies の id) とバイト数 */
    long fid;               /* プログラムボードのファイル (files の id、0 は無し) */
    int reads, dls;
    bool deleted;
    char fname[16];
    long fsize;
    int to[MAX_MAIL_TO];    /* メールの宛先 (0 は空き) */
    char to_state[MAX_MAIL_TO]; /* n 未読 / r 既読 / d 削除 */
};

struct logent {
    time_t in, out;
    int line;
    int id;
    char logname[64];
    char speed[24];
};

struct application {
    int no;
    time_t t;
    char name[64], kana[64], addr[128], zip[16], tel[32], pass[32];
    char logname[64];       /* ハンドル名 (申し込みで聞いたとき) */
    char x_account[32];     /* X のアカウント (@ なし) */
    int issued_id;          /* 発行済みなら ID */
};

enum signup_mode { SIGNUP_AUTO = 0, SIGNUP_MANUAL = 1, SIGNUP_OFFLINE = 2 };

struct system {
    char net_id[8];
    int min_minutes;        /* 最低持ち時間 (ゲスト) */
    int user_minutes;       /* ユーザ持ち時間 */
    int user_level;         /* 一般会員のレベル */
    int temp_level;         /* 仮 ID のレベル */
    int chat_level;
    int mail_size;
    long board_size;        /* 総ボードサイズ: 本文の合計の上限 (バイト) */
    long pds_size;          /* 総 PDS サイズ: プログラムボードのファイルの合計の上限 (バイト) */
    int signup;
    int manager;            /* 会員管理者の ID */
    bool aoff;              /* 新規ログイン禁止 */
    long total_logins, guest_logins;
    time_t started;
    char sysmes[10][1024];  /* MESEDIT のメッセージ 0〜9 */
    /* LINESET: 回線ごとのアクセスできるレベルと時間のモード */
    struct { int level, mode; } lines[MAX_LINES + 1];
    /* CTIME: 時間で制限する回線と、曜日 × 時の禁止表 (1 = 禁止) */
    bool chat_limited[MAX_LINES + 1];
    unsigned char ctime_tab[7][24];
    bool busy_line[MAX_LINES + 1];  /* LBUSY: BUSY 時間を数える回線 */
    int chat_call_level;            /* CMODE: チャットコールを受けるレベル (255 = OFF) */
    /* IMODE: 新しい ID の初期モード */
    bool imode_menu, imode_menu_always, imode_chat_esc, imode_chat_noecho, imode_chat_self, imode_chat_byid;
    bool keylock;
    long busy_sec;                  /* 本日の BUSY 時間 (秒) */
    int busy_day;
    time_t down_at;                 /* SDTIME: システムを止める時刻 (0 は無し。保存しない) */
};

enum { LM_NORMAL = 0, LM_NOTIMEOUT = 1, LM_ALWAYS = 2 };

/* スケジュール (SCSET) */
enum { SK_DATE = 0, SK_DAILY = 1, SK_WEEKDAY = 2 };
struct sched {
    int no;
    bool spot;              /* false: 期間型 (オープニングに出す)、true: スポット型 (その時刻に全回線へ) */
    char start[12], end[12];/* 期間型: YYYY-MM-DD */
    int kind;               /* スポット型: SK_DATE / SK_DAILY / SK_WEEKDAY */
    char date[12];
    int wday;
    char hhmm[8];
    bool down;              /* システムダウンする */
    int timer_wday;
    char timer_hhmm[8];
    char msg[200];
};

struct sched *sched_list(int *n);
int sched_add(const struct sched *s);
void sched_del(int index);

/* 全部のログ (古い順)。統計 (ACCESS) 用 */
int log_all(struct logent *out, int max);
void log_remove_user(int id);

extern struct user *g_users;      /* [MAX_USERS] */
extern int g_nusers;              /* 使った最大の ID + 1 */
extern struct board g_boards[MAX_BOARDS];
extern struct msg *g_msgs;
extern int g_nmsgs;
extern struct system g_sys;

int db_open(void);
void db_save_all(void);
void db_save_users(void);
void db_save_boards(void);
void db_save_msgs(void);
void db_save_ptrs(void);
void db_save_sys(void);

struct user *user_get(int id);                  /* 存在し削除されていない会員 */
struct user *user_find(const char *id_or_name); /* ID 番号かログネームで */
int user_new(int level);                        /* 新しい ID を発行 */
int user_new_at(int id, int level);
const char *level_name(int level);

int *read_ptr(int user, int board);             /* 既読位置 (最後に読んだ番号) */
char *bset_flag(int user, int board);           /* RALL の対象 'Y'/'N' */
char *cug_right(int user, int board);           /* CUG 権 a/r/w/n */

bool board_can_read(const struct user *u, const struct board *b);
bool board_can_write(const struct user *u, const struct board *b);
struct board *board_by_index(const char *idx);
bool time_forbidden(void);

struct msg *msg_get(int board, int seq);
int msg_add(struct msg *m, const char *body);   /* 番号を振って追加。seq を返す */
char *msg_body(const struct msg *m);            /* malloc した本文 */
int board_count(int board);
long total_body_bytes(void);
long total_pds_bytes(void);
int board_unread(int user, int board);
bool mail_is_for(const struct msg *m, int user, int *slot);

void log_add(const struct logent *e);
int log_recent(struct logent *out, int max);

int app_add(const struct application *a);
struct application *app_list(int *n);
void app_save(void);

long file_add(const void *data, size_t len);    /* ファイル本体を保存して id を返す */
void *file_get(long fid, size_t *len);          /* malloc したファイル本体 */
int filem(void);                                /* ファイルメンテナンス。消した件数 */

#endif
