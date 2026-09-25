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

#define MAX_MODEMS 8
#define MAX_INIT 8

/* モデム回線 1 本の設定 (設定ファイルでは modem<回線番号>.<項目> = 値) */
struct modem_cfg {
    int line;
    char path[256];
    int baud;               /* DTE 速度 */
    char flow[16];          /* none / hardware / software */
    char init[MAX_INIT][128];
    int ninit;
    bool answer_auto;       /* true: モデムの自動着信 (S0=1)、false: RING で ATA */
    int rings;              /* 何回目の RING で ATA するか */
    char carrier[8];        /* dcd / text / both */
    char hangup[8];         /* dtr / escape */
    int connect_timeout;    /* 秒 */
    int connect_delay_ms;
    char code[8];           /* 文字コード (空なら default_code) */
};

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
    char mes_esc_file[512]; /* NET-COCK の MES_ESC.TXT (ESC を使う会員に出す) */
    struct modem_cfg modems[MAX_MODEMS];
    int nmodems;
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
    bool binary;            /* バイナリ転送中 (CR の処理をしない) */
    bool telnet;            /* TCP (telnet) の回線。false はシリアル (モデム) */
    int carrier;            /* シリアルのキャリア断の検出: CARRIER_DCD / CARRIER_TEXT */
    char tail[16];          /* 「NO CARRIER」を探すための受信の末尾 */
    bool warned;            /* 持ち時間の予告を出した */
    bool warn_pending;
};

enum read_flags { RL_MASK = 1, RL_UPPER = 2, RL_RAW = 4 };
enum { CARRIER_DCD = 1, CARRIER_TEXT = 2 };

/* term_readline などの戻り値 */
#define T_OK 0
#define T_DISCONNECT (-1)
#define T_TIMEOUT (-2)
#define T_TIMEUP (-3)
#define T_KICKED (-4)

void term_init(struct term *t, int fd, int no, int notify_rd, const char *code, bool telnet);
void term_set_code(struct term *t, enum term_code code);
void term_free(struct term *t);
int term_write_raw(struct term *t, const void *buf, size_t len);
int term_print(struct term *t, const char *s);
int term_printf(struct term *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int term_readline(struct term *t, const char *prompt, char *out, size_t outsz, int flags);
int term_more(struct term *t, int *line_count);
int term_yesno(struct term *t, const char *prompt);
/* バイナリ転送用: 1 バイト読む (0〜255。時間切れは T_NODATA)、IAC をエスケープして書く */
#define T_NODATA (-100)
int term_getc(struct term *t, int timeout_ms);
int term_write_bin(struct term *t, const void *buf, size_t len);
void term_set_binary(struct term *t, bool on);
void term_purge(struct term *t, int quiet_ms);

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

int online_alloc(const char *peer);          /* TCP 用に空き回線を取る (モデムの回線は除く) */
int online_alloc_at(int no, const char *peer); /* 決まった回線 (モデム) を取る */
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
/* 表示幅が width になるよう空白を足した文字列を buf に作る (全角は 2 桁と数える) */
const char *pad(char *buf, size_t sz, const char *s, int width);
void str_upper(char *s);
void str_trim(char *s);
void fmt_time(time_t t, char *buf, size_t sz);

/* ------------------------------------------------------------ セッション */

struct conn_arg {
    int fd;
    int no;
    char peer[64];
    char speed[32];         /* LOG に残す速度 (TCP / 2400/V42BIS など) */
    bool serial;
    int carrier;
    char code[8];
};

void *session_thread(void *arg);   /* TCP: セッションが終わったら切断して回線を返す */
void session_run(struct conn_arg *ca); /* セッションだけ (モデムの回線から呼ぶ) */

/* ------------------------------------------------------------ モデム */

void modem_start(void);

#endif
