/*
 * ホストコンソール (0 回線)
 *
 * 起動した端末 (標準入力が端末のとき) からコマンドで操作する。X68000 版のファンクションキーの代わり:
 *   cock        ホストからログインする (文字列は設定の host_access)
 *   mon N       N 回線目の画面を監視する (Enter で終わる。メールと会員データのコマンド中は見せない)
 *   op N        監視しながら代わりに操作する (~. で終わる)
 *   kick N      N 回線目を切る
 *   lines       回線の一覧
 *   bcast 文    全回線に知らせる
 *   lock        キーボードロック (外すのは外線からの KEYLOCK)
 *   call        チャットコールのモードを順に切り替える
 *   quit        保存して終了する (誰もログインしていないときだけ)
 */
#include "db.h"
#include "msg.h"
#include "nc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void show_lines(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i <= g_cfg.max_lines; i++) {
        struct online *o = &g_online[i];
        if (!o->used) {
            if (i > 0) say("%2d  待機中\n", i);
            continue;
        }
        long m = (long)(now - o->since) / 60;
        if (o->uid < 0) say("%2d  (ログイン前)  %s\n", i, o->peer);
        else
            say("%2d  %-8s %04d:%-16s %3ld:%02ld  残り %5d%s  %s\n", i, o->place, o->uid, o->handle, m / 60, m % 60,
                o->left, o->counting ? "*" : " ", o->peer);
    }
    pthread_mutex_unlock(&g_lock);
}

static bool anyone_online(void) {
    bool any = false;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i <= MAX_LINES; i++)
        if (g_online[i].used && g_online[i].uid >= 0) any = true;
    pthread_mutex_unlock(&g_lock);
    return any;
}

/* ホストからログインする。端末を 1 文字ずつ読むモードにしてセッションを動かす */
static void host_login(void) {
    if (!online_alloc_at(0, "host")) {
        say("0 回線は使用中です\n");
        return;
    }
    struct termios saved, raw;
    bool tty = tcgetattr(0, &saved) == 0;
    if (tty) {
        raw = saved;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
        raw.c_iflag &= ~(tcflag_t)(ICRNL | IXON);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
    }
    struct conn_arg ca = {.fd = 0, .out_fd = 1, .no = 0, .serial = true, .host = true};
    snprintf(ca.peer, sizeof ca.peer, "host");
    snprintf(ca.speed, sizeof ca.speed, "host");
    snprintf(ca.code, sizeof ca.code, "utf8");
    session_run(&ca);
    online_free(0);
    if (tty) tcsetattr(0, TCSANOW, &saved);
    say("\nホストのセッションを終わりました\n");
}

static int line_arg(const char *p) {
    int n = atoi(p);
    return n >= 1 && n <= MAX_LINES ? n : -1;
}

static void monitor(int no, bool operate) {
    pthread_mutex_lock(&g_lock);
    bool used = g_online[no].used;
    if (used) g_online[no].monitor_fd = 1;
    pthread_mutex_unlock(&g_lock);
    if (!used) {
        say("%d 回線は使われていません\n", no);
        return;
    }
    say("--- %d 回線を%s (%s で終わり) ---\n", no, operate ? "操作します" : "監視します", operate ? "~." : "Enter");
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!operate || !strcmp(line, "~.")) break;
        strcat(line, "\r");
        online_inject(no, line);
    }
    pthread_mutex_lock(&g_lock);
    g_online[no].monitor_fd = -1;
    pthread_mutex_unlock(&g_lock);
    say("--- 終わり ---\n");
}

static void help(void) {
    say("%s: ホストからログイン / lines: 回線の一覧 / mon N: 監視 / op N: 代わりに操作 (~. で終わり)\n"
        "kick N: 回線を切る / bcast 文: 全回線に知らせる / lock: キーボードロック / call: チャットコールの切り替え\n"
        "quit: 保存して終了 (誰もログインしていないとき)\n",
        g_cfg.host_access);
}

static void *console_thread(void *arg) {
    say("ホストコンソール: help でコマンドの一覧\n");
    char line[512];
    for (;;) {
        say("host> ");
        if (!fgets(line, sizeof line, stdin)) return NULL; /* 端末が閉じた */
        line[strcspn(line, "\r\n")] = 0;
        str_trim(line);
        if (!line[0]) continue;
        pthread_mutex_lock(&g_lock);
        bool locked = g_sys.keylock;
        pthread_mutex_unlock(&g_lock);
        if (locked) {
            say("%s%s (外線から KEYLOCK で外してください)\n", M(386), M(388));
            continue;
        }
        char cmd[32];
        const char *arg = line + strcspn(line, " ");
        snprintf(cmd, sizeof cmd, "%.*s", (int)(arg - line), line);
        while (*arg == ' ') arg++;
        if (!strcmp(cmd, g_cfg.host_access)) host_login();
        else if (!strcmp(cmd, "lines")) show_lines();
        else if (!strcmp(cmd, "mon") || !strcmp(cmd, "op")) {
            int n = line_arg(arg);
            if (n > 0) monitor(n, cmd[0] == 'o');
        } else if (!strcmp(cmd, "kick")) {
            int n = line_arg(arg);
            if (n > 0 && notice_push(n, N_KICK, "ホストが回線を切りました")) {
                nc_log("ホスト: %d 回線を切りました", n);
            } else say("%s は使われていません\n", arg);
        } else if (!strcmp(cmd, "bcast") && *arg) {
            char text[400];
            snprintf(text, sizeof text, "*** ホストより: %s ***", arg);
            say("%d 回線に知らせました\n", notice_broadcast(N_SYSTEM, text, -1));
        } else if (!strcmp(cmd, "lock")) {
            pthread_mutex_lock(&g_lock);
            g_sys.keylock = true;
            db_save_sys();
            pthread_mutex_unlock(&g_lock);
            say("%s\n", M(385));
        } else if (!strcmp(cmd, "call")) {
            static const int levels[] = {0, 30, 40, 80, 100, 255};
            static const char *const names[] = {"ON", "ON L", "ON M", "ON H", "ON S", "OFF"};
            pthread_mutex_lock(&g_lock);
            int k = 0;
            while (k < 6 && levels[k] != g_sys.chat_call_level) k++;
            k = (k + 1) % 6;
            g_sys.chat_call_level = levels[k];
            db_save_sys();
            pthread_mutex_unlock(&g_lock);
            say("チャットコール: %s\n", names[k]);
        } else if (!strcmp(cmd, "quit")) {
            if (anyone_online()) {
                say("ログインしている人がいるので終了できません\n");
                continue;
            }
            pthread_mutex_lock(&g_lock);
            db_save_all();
            nc_log("ホストコンソールから終了します");
            exit(0);
        } else help();
    }
}

void console_start(void) {
    if (!g_cfg.console || !isatty(0)) return;
    pthread_t th;
    if (pthread_create(&th, NULL, console_thread, NULL) == 0) pthread_detach(th);
}
