/*
 * 設定ファイル・ログ・文字列の小物
 */
#include "nc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct config g_cfg = {
    .bbs_name = "NULL-NET",
    .net_id = "NULL",
    .data_dir = "data",
    .listen = "0.0.0.0:6868",
    .max_lines = 16,
    .idle_timeout = 300,
    .session_minutes = 60,
    .guest_minutes = 15,
    .default_code = "sjis",
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* 「キー = 値」の行を読む。# 以降はコメント */
int config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        str_trim(key);
        str_trim(val);
        if (val[0] == '"') {
            val++;
            char *q = strrchr(val, '"');
            if (q) *q = 0;
        }
#define STR(name) if (!strcmp(key, #name)) snprintf(g_cfg.name, sizeof g_cfg.name, "%s", val);
#define INT(name) if (!strcmp(key, #name)) g_cfg.name = atoi(val);
        STR(bbs_name) STR(net_id) STR(data_dir) STR(listen) STR(default_code)
        INT(max_lines) INT(idle_timeout) INT(session_minutes) INT(guest_minutes)
#undef STR
#undef INT
    }
    fclose(f);
    if (g_cfg.max_lines < 1) g_cfg.max_lines = 1;
    if (g_cfg.max_lines > MAX_LINES) g_cfg.max_lines = MAX_LINES;
    return 0;
}

void nc_log(const char *fmt, ...) {
    char msg[1024], ts[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%m/%d %H:%M:%S", localtime(&now));
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "%s %s\n", ts, msg);
    char path[600];
    snprintf(path, sizeof path, "%s/system.log", g_cfg.data_dir);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s %s\n", ts, msg);
        fclose(f);
    }
    pthread_mutex_unlock(&log_lock);
}

/* UTF-8 の表示幅 (全角 2、半角カナ 1) */
size_t utf8_width(const char *s) {
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p < 0x80) { w++; p++; continue; }
        size_t len = *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
        /* 半角カナ U+FF61〜U+FF9F は EF BD A1〜EF BE 9F */
        bool hankana = len == 3 && p[0] == 0xEF &&
                       ((p[1] == 0xBD && p[2] >= 0xA1) || (p[1] == 0xBE && p[2] <= 0x9F));
        w += hankana ? 1 : 2;
        for (size_t i = 0; i < len && *p; i++) p++;
    }
    return w;
}

void str_upper(char *s) {
    for (; *s; s++) if ((unsigned char)*s < 0x80) *s = (char)toupper((unsigned char)*s);
}

void str_trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

/* 「90/10/31 04:58」の形 */
void fmt_time(time_t t, char *buf, size_t sz) {
    strftime(buf, sz, "%y/%m/%d %H:%M", localtime(&t));
}
