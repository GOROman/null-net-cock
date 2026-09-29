/*
 * FILEM の単体テスト: db.c だけをつないで、削除済みの取り除きと最低保存数 (keep) の扱いを確かめる
 *   cc -std=c11 -Isrc -D_DEFAULT_SOURCE -o build/filem_test tests/filem_test.c src/db.c src/util.c -lpthread -lsqlite3
 */
#include "../src/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("NG: %s (%d 行目)\n", #c, __LINE__); fails++; } } while (0)

static void post(int board, int reply_to, const char *title, const char *body, time_t t) {
    struct msg m = {.board = board, .from = 1, .reply_to = reply_to};
    snprintf(m.title, sizeof m.title, "%s", title);
    msg_add(&m, body);
    msg_get(board, m.seq)->t = t;
}

static int alive(int board) { return board_count(board); }

int main(void) {
    char dir[] = "/tmp/filem-test-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    snprintf(g_cfg.data_dir, sizeof g_cfg.data_dir, "%s", dir);
    if (db_open() < 0) return 2;
    struct board *b1 = &g_boards[1], *b2 = &g_boards[2];
    b1->used = b2->used = true;
    b1->no = 1;
    b2->no = 2;
    b1->type = b2->type = BT_BOARD;
    b1->keep = 3;   /* 最低 3 件 */
    b2->keep = 0;   /* 削除なし */
    b1->next_seq = b2->next_seq = 1;

    char body[1001];
    memset(body, 'a', 1000);
    body[1000] = 0;
    for (int i = 1; i <= 10; i++) {
        char t[32];
        snprintf(t, sizeof t, "b1-%d", i);
        post(1, i == 5 ? 2 : i == 9 ? 4 : 0, t, body, 1000 + i);  /* 5 番は 2 番へ、9 番は 4 番へのリプライ */
        snprintf(t, sizeof t, "b2-%d", i);
        post(2, 0, t, body, 2000 + i);
    }
    msg_get(1, 2)->deleted = true;                       /* 親を削除 */
    msg_get(2, 7)->deleted = true;

    /* 1. 容量に余裕がある: 消えるのは削除済みの 2 件だけ。keep (3) を超えて残っていてもそのまま */
    g_sys.board_size = 16L * 1024 * 1024;
    CHECK(filem() == 2);
    CHECK(alive(1) == 9 && alive(2) == 9);
    /* 1 番は 1 番、旧 3 番 → 新 2 番。リプライだった旧 5 番 → 新 4 番は残り、返信先は外れる */
    struct msg *m = msg_get(1, 4);
    CHECK(m && strcmp(m->title, "b1-5") == 0 && m->reply_to == 0);
    /* 親も子も残るリプライ: 9 番 → 8 番、返信先の 4 番 → 3 番 */
    m = msg_get(1, 8);
    CHECK(m && strcmp(m->title, "b1-9") == 0 && m->reply_to == 3);
    CHECK(strcmp(msg_get(1, 3)->title, "b1-4") == 0);
    m = msg_get(1, 2);
    CHECK(m && strcmp(m->title, "b1-3") == 0);
    char *bd = m ? msg_body(m) : NULL;
    CHECK(bd && strlen(bd) == 1000);
    free(bd);
    CHECK(msg_get(2, 7) && strcmp(msg_get(2, 7)->title, "b2-8") == 0);

    /* 2. 容量を超えている: 古いものから外すが、keep が 0 のボードには手を付けず、keep (3) より減らさない */
    g_sys.board_size = 6000;   /* 今は 18 件 = 18000 バイト */
    filem();
    CHECK(alive(2) == 9);
    CHECK(alive(1) == 3);
    CHECK(strcmp(msg_get(1, 1)->title, "b1-8") == 0 && strcmp(msg_get(1, 3)->title, "b1-10") == 0);
    CHECK(strcmp(msg_get(1, 2)->title, "b1-9") == 0 && msg_get(1, 2)->reply_to == 0);  /* 親 (4 番) が外れた */
    CHECK(total_body_bytes() == 12000);   /* keep のせいでこれ以上は減らせない */

    /* 3. もう一度実行しても変わらない */
    CHECK(filem() == 0);
    CHECK(alive(1) == 3 && alive(2) == 9);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
    if (fails) return 1;
    printf("OK: FILEM のテスト通過\n");
    return 0;
}
