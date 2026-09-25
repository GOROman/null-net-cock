/*
 * null-net-cock: 起動と TCP の待ち受け
 */
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
    mkdir(g_cfg.data_dir, 0755);

    int ls = listen_tcp(g_cfg.listen);
    if (ls < 0) {
        fprintf(stderr, "%s で待ち受けできません: %s\n", g_cfg.listen, strerror(errno));
        return 1;
    }
    nc_log("%s (null-net-cock %s) を起動しました。TCP %s / 最大 %d 回線", g_cfg.bbs_name, NC_VERSION,
           g_cfg.listen, g_cfg.max_lines);

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
        struct conn_arg *ca = malloc(sizeof *ca);
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
