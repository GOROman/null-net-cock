/*
 * セッション (接続からログオフまで)。コマンド体系は仕様がまとまってから実装する
 */
#include "nc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *session_thread(void *arg) {
    struct conn_arg *ca = arg;
    struct term t;
    term_init(&t, ca->fd, ca->no, g_online[ca->no].notify_rd, g_cfg.default_code);
    nc_log("CH%02d: 接続 %s", ca->no, ca->peer);
    term_printf(&t, "\n%s (null-net-cock %s)\n", g_cfg.bbs_name, NC_VERSION);
    char buf[256];
    for (;;) {
        int r = term_readline(&t, "COMMAND> ", buf, sizeof buf, RL_UPPER);
        if (r < 0) break;
        if (!strcmp(buf, "BYE") || !strcmp(buf, "G")) {
            term_print(&t, "ご利用ありがとうございました。\n");
            break;
        }
        term_printf(&t, "「%s」(表示幅 %zu)\n", buf, utf8_width(buf));
    }
    nc_log("CH%02d: 切断 %s", ca->no, ca->peer);
    term_free(&t);
    close(ca->fd);
    online_free(ca->no);
    free(ca);
    return NULL;
}
