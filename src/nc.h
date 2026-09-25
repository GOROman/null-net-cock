/*
 * null-net-cock: NET-COCK 互換のパソコン通信ホストプログラム (C 言語による新規実装)
 *
 * 1 回線 = 1 スレッド。セッションの処理は上から順に書き、共有データは g_lock で守る。
 */
#ifndef NC_H
#define NC_H

#include <iconv.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define NC_VERSION "0.1.0"
#define MAX_LINES 128
#define ID_LEN 16
#define HANDLE_LEN 64
#define LINE_MAX_BYTES 1024

/* ------------------------------------------------------------ 設定 */

struct config {
    char bbs_name[128];
    char net_id[16];        /* ネットワーク ID (例: COCK) */
    char data_dir[512];
    char listen[128];       /* TCP の待ち受け (例: 0.0.0.0:6868) */
    int max_lines;
    int idle_timeout;       /* 無操作で切断するまでの秒数 */
    int session_minutes;    /* 1 回の接続の上限 (分) */
    int guest_minutes;
    char default_code[8];   /* 端末の既定の文字コード: sjis / utf8 */
    char mes_file[512];     /* NET-COCK の MES.TXT (指定すると元と同じ文言になる) */
    char sysmes_file[512];  /* NET-COCK の SYS_MES.DAT */
};

extern struct config g_cfg;
extern pthread_mutex_t g_lock;

int config_load(const char *path);

/* ------------------------------------------------------------ 端末 (1 回線ぶんの入出力) */

enum term_code { CODE_SJIS, CODE_UTF8 };

struct term {
    int fd;
    int no;                 /* 回線番号 (1〜) */
    int notify_rd;          /* 通知を知らせるパイプ (読み側) */
    enum term_code code;
    iconv_t to_term;        /* UTF-8 → 端末 */
    iconv_t from_term;      /* 端末 → UTF-8 */
    unsigned char in[4096]; /* 受信済みで未処理のバイト */
    size_t in_len;
    int telnet_state;
    bool after_cr;
    time_t last_input;
    time_t deadline;        /* 接続時間の上限 (0 は無制限) */
    int rows;               /* 1 ページの行数 */
    bool closed;
};

enum read_flags { RL_MASK = 1, RL_UPPER = 2, RL_RAW = 4 };

/* term_readline などの戻り値 */
#define T_OK 0
#define T_DISCONNECT (-1)
#define T_TIMEOUT (-2)
#define T_TIMEUP (-3)
#define T_KICKED (-4)

void term_init(struct term *t, int fd, int no, int notify_rd, const char *code);
void term_set_code(struct term *t, enum term_code code);
void term_free(struct term *t);
int term_write_raw(struct term *t, const void *buf, size_t len);
int term_print(struct term *t, const char *s);
int term_printf(struct term *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int term_readline(struct term *t, const char *prompt, char *out, size_t outsz, int flags);
int term_more(struct term *t, int *line_count);
int term_yesno(struct term *t, const char *prompt);

/* ------------------------------------------------------------ 在室者と通知 */

enum notice_kind { N_TELEGRAM, N_CHAT, N_SYSTEM, N_KICK };

struct online {
    bool used;
    int no;
    char id[ID_LEN];
    char handle[HANDLE_LEN];
    char place[64];
    char peer[64];
    time_t since;
    bool chat;              /* チャットを受け付ける */
    bool secret;            /* 極秘モード (LLIST に出さない) */
    int notify_wr;
    int notify_rd;
    /* 届いた通知 (リングバッファ) */
    struct { enum notice_kind kind; char text[512]; } q[32];
    int q_head, q_len;
};

extern struct online g_online[MAX_LINES + 1];

int online_alloc(const char *peer);
void online_free(int no);
void online_set_user(int no, const char *id, const char *handle);
void online_set_place(int no, const char *place);
bool notice_push(int no, enum notice_kind kind, const char *text);
bool notice_pop(int no, enum notice_kind *kind, char *text, size_t sz);
int notice_broadcast(enum notice_kind kind, const char *text, int except);

/* ------------------------------------------------------------ ログ */

void nc_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ------------------------------------------------------------ 文字列 */

size_t utf8_width(const char *s);
void str_upper(char *s);
void str_trim(char *s);
void fmt_time(time_t t, char *buf, size_t sz);

/* ------------------------------------------------------------ セッション */

struct conn_arg {
    int fd;
    int no;
    char peer[64];
};

void *session_thread(void *arg);

#endif
