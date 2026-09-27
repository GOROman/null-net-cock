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
    .net_id = "TEST",
    .data_dir = "data",
    .listen = "0.0.0.0:6868",
    .max_lines = 16,
    .idle_timeout = 300,
    .session_minutes = 60,
    .guest_minutes = 15,
    .default_code = "sjis",
    .host_access = "cock",
    .signup_fields = SF_HANDLE,
    .profile_fields = 0,
    .console = true,
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* 「handle,x」のような並びを項目の集合にする。netcock は元の NET-COCK と同じ聞き方 (netcock_bits) */
int parse_fields(const char *list, int netcock_bits) {
    static const struct { const char *name; int bit; } names[] = {
        {"name", SF_NAME}, {"kana", SF_KANA}, {"addr", SF_ADDR}, {"zip", SF_ZIP}, {"tel", SF_TEL},
        {"handle", SF_HANDLE}, {"pub_addr", SF_PUB_ADDR}, {"job", SF_JOB}, {"machine", SF_MACHINE},
        {"birth", SF_BIRTH}, {"sex", SF_SEX}, {"term", SF_TERM}, {"intro", SF_INTRO}, {"x", SF_X},
    };
    int bits = 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *save, *p = strtok_r(buf, ", ", &save); p; p = strtok_r(NULL, ", ", &save)) {
        if (!strcmp(p, "netcock")) bits |= netcock_bits;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!strcmp(p, names[i].name)) bits |= names[i].bit;
    }
    return bits;
}

/* modem<回線>.<項目> = 値 */
static void modem_key(int line, const char *dot, const char *val) {
    if (!dot || line < 1 || line > MAX_LINES) return;
    const char *k = dot + 1;
    struct modem_cfg *m = NULL;
    for (int i = 0; i < g_cfg.nmodems; i++)
        if (g_cfg.modems[i].line == line) m = &g_cfg.modems[i];
    if (!m) {
        if (g_cfg.nmodems >= MAX_MODEMS) return;
        m = &g_cfg.modems[g_cfg.nmodems++];
        *m = (struct modem_cfg){.line = line, .baud = 9600, .flow = "hardware", .rings = 1, .carrier = "both",
                                .hangup = "dtr", .connect_timeout = 60, .connect_delay_ms = 500};
    }
    if (!strcmp(k, "path")) snprintf(m->path, sizeof m->path, "%s", val);
    else if (!strcmp(k, "baud")) m->baud = atoi(val);
    else if (!strcmp(k, "flow")) snprintf(m->flow, sizeof m->flow, "%s", val);
    else if (!strcmp(k, "answer")) m->answer_auto = !strcmp(val, "auto");
    else if (!strcmp(k, "rings")) m->rings = atoi(val);
    else if (!strcmp(k, "carrier")) snprintf(m->carrier, sizeof m->carrier, "%s", val);
    else if (!strcmp(k, "hangup")) snprintf(m->hangup, sizeof m->hangup, "%s", val);
    else if (!strcmp(k, "connect_timeout")) m->connect_timeout = atoi(val);
    else if (!strcmp(k, "connect_delay_ms")) m->connect_delay_ms = atoi(val);
    else if (!strcmp(k, "code")) snprintf(m->code, sizeof m->code, "%s", val);
    else if (!strcmp(k, "init")) {
        /* 「;」で区切って複数書ける */
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", val);
        m->ninit = 0;
        for (char *save, *p = strtok_r(buf, ";", &save); p && m->ninit < MAX_INIT; p = strtok_r(NULL, ";", &save)) {
            str_trim(p);
            if (*p) snprintf(m->init[m->ninit++], sizeof m->init[0], "%s", p);
        }
    }
}

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
        if (!strncmp(key, "modem", 5) && isdigit((unsigned char)key[5])) {
            modem_key(atoi(key + 5), strchr(key, '.'), val);
            continue;
        }
#define STR(name) if (!strcmp(key, #name)) snprintf(g_cfg.name, sizeof g_cfg.name, "%s", val);
#define INT(name) if (!strcmp(key, #name)) g_cfg.name = atoi(val);
        STR(bbs_name) STR(net_id) STR(data_dir) STR(listen) STR(default_code) STR(mes_file) STR(sysmes_file) STR(mes_esc_file) STR(host_access) STR(help_file) STR(ws_listen) STR(ws_code)
        if (!strcmp(key, "signup_fields")) g_cfg.signup_fields = parse_fields(val, SF_NETCOCK_SIGNUP);
        if (!strcmp(key, "profile_fields")) g_cfg.profile_fields = parse_fields(val, SF_NETCOCK_PROFILE);
        if (!strcmp(key, "console")) g_cfg.console = !strcmp(val, "on") || !strcmp(val, "1") || !strcmp(val, "yes");
        INT(max_lines) INT(idle_timeout) INT(session_minutes) INT(guest_minutes)
#undef STR
#undef INT
    }
    fclose(f);
    if (g_cfg.max_lines < 1) g_cfg.max_lines = 1;
    if (g_cfg.max_lines > MAX_LINES) g_cfg.max_lines = MAX_LINES;
    for (int i = 0; i < g_cfg.nmodems; i++) {
        struct modem_cfg *m = &g_cfg.modems[i];
        if (!m->ninit) {
            snprintf(m->init[0], sizeof m->init[0], "ATZ");
            snprintf(m->init[1], sizeof m->init[1], "ATE0V1Q0X4&C1&D2S0=%d", m->answer_auto ? 1 : 0);
            m->ninit = 2;
        }
        if (m->line > g_cfg.max_lines) g_cfg.max_lines = m->line;
    }
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

const char *pad(char *buf, size_t sz, const char *s, int width) {
    int w = (int)utf8_width(s);
    snprintf(buf, sz, "%s%*s", s, w < width ? width - w : 0, "");
    return buf;
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
