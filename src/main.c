/*
 * null-net-cock: 起動と TCP の待ち受け
 */
#include "db.h"
#include "msg.h"
#include "nc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int listen_tcp(const char *spec) {
    char host[64] = "0.0.0.0";
    int port = 6868;
    const char *colon = strrchr(spec, ':');
    if (colon) {
        snprintf(host, sizeof host, "%.*s", (int)(colon - spec), spec);
        port = atoi(colon + 1);
    } else {
        port = atoi(spec);
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 || bind(s, (struct sockaddr *)&a, sizeof a) < 0 ||
        listen(s, 16) < 0) {
        close(s);
        return -1;
    }
    return s;
}

/* SIGTERM / SIGINT を受けたら、データを保存して終わる */
static void *signal_thread(void *arg) {
    sigset_t *set = arg;
    int sig;
    sigwait(set, &sig);
    pthread_mutex_lock(&g_lock);
    db_save_all();
    nc_log("シグナル %d を受けたので、データを保存して終了します", sig);
    exit(0);
}

static void usage(const char *prog) {
    fprintf(stderr, "使い方: %s [-c 設定ファイル]\n", prog);
    exit(2);
}

int main(int argc, char **argv) {
    const char *cfg_path = "null-net-cock.conf";
    int opt;
    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        if (opt == 'c') cfg_path = optarg;
        else usage(argv[0]);
    }
    if (config_load(cfg_path) < 0 && strcmp(cfg_path, "null-net-cock.conf") != 0) {
        fprintf(stderr, "%s を読めません\n", cfg_path);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    /* 以後に作るスレッドも含めて SIGTERM / SIGINT は signal_thread だけが受ける */
    static sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    pthread_t sth;
    pthread_create(&sth, NULL, signal_thread, &sigs);
    mkdir(g_cfg.data_dir, 0755);

    if (db_open() < 0) {
        fprintf(stderr, "%s/net-cock.db を開けません\n", g_cfg.data_dir);
        return 1;
    }
    /* LCSET の「次のシステムダウンまで」は起動し直したら消す */
    for (int i = 0; i < g_nusers; i++)
        if (g_users[i].lcall == 1) g_users[i].lcall = 0;
    if (g_cfg.mes_file[0]) {
        int n = msg_load(g_cfg.mes_file);
        if (n < 0) fprintf(stderr, "%s を読めません。内蔵の文言を使います\n", g_cfg.mes_file);
        else nc_log("%s から %d 件の文言を読み込みました", g_cfg.mes_file, n);
    }
    if (g_cfg.mes_esc_file[0]) {
        int n = msg_load_esc(g_cfg.mes_esc_file);
        if (n < 0) fprintf(stderr, "%s を読めません\n", g_cfg.mes_esc_file);
        else nc_log("%s から %d 件の ESC 版の文言を読み込みました", g_cfg.mes_esc_file, n);
    }
    /* MESEDIT のメッセージがまだ無ければ、SYS_MES.DAT か既定の文面で埋める */
    if (!g_sys.sysmes[0][0]) {
        if (g_cfg.sysmes_file[0] && sysmes_load(g_cfg.sysmes_file) > 0)
            nc_log("%s を読み込みました", g_cfg.sysmes_file);
        else {
            static const char *def[10] = {
                "", "", "", "ゲストでログインしました。", "ご利用ありがとうございました。",
                "? でコマンドの一覧を表示します。", "", "", "", "",
            };
            snprintf(g_sys.sysmes[0], sizeof g_sys.sysmes[0], "%s (null-net-cock %s)\nゲストは GUEST と入力してください。",
                     g_cfg.bbs_name, NC_VERSION);
            for (int i = 1; i < 10; i++) snprintf(g_sys.sysmes[i], sizeof g_sys.sysmes[i], "%s", def[i]);
        }
        db_save_sys();
    }

    int ls = listen_tcp(g_cfg.listen);
    if (ls < 0) {
        fprintf(stderr, "%s で待ち受けできません: %s\n", g_cfg.listen, strerror(errno));
        return 1;
    }
    nc_log("%s (null-net-cock %s) を起動しました。TCP %s / 最大 %d 回線", g_cfg.bbs_name, NC_VERSION,
           g_cfg.listen, g_cfg.max_lines);
    modem_start();
    monitor_start();
    console_start();

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int fd = accept(ls, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            nc_log("accept: %s", strerror(errno));
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        char addr[64];
        snprintf(addr, sizeof addr, "%s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        int no = online_alloc(addr);
        if (!no) {
            static const char busy[] = "\r\nただいま回線が混み合っています。\r\n";
            (void)!write(fd, busy, sizeof busy - 1);
            close(fd);
            nc_log("%s: 回線が満杯のため断りました", addr);
            continue;
        }
        struct conn_arg *ca = calloc(1, sizeof *ca);
        ca->fd = fd;
        ca->no = no;
        snprintf(ca->peer, sizeof ca->peer, "%s", addr);
        pthread_t th;
        if (pthread_create(&th, NULL, session_thread, ca) != 0) {
            close(fd);
            online_free(no);
            free(ca);
            continue;
        }
        pthread_detach(th);
    }
}
