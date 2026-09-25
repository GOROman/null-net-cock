#ifndef MSG_H
#define MSG_H

#include <stdbool.h>

/* ID で文言を引く (ID の割り当ては NET-COCK の MES.TXT と同じ) */
const char *M(int id);
/* NET-COCK の MES.TXT を読み込む。読めたメッセージ数を返す */
int msg_load(const char *path);
/* NET-COCK の SYS_MES.DAT を読み込む */
int sysmes_load(const char *path);
/* NET-COCK の MES_ESC.TXT (ESC シーケンス入りの文言) を読み込む */
int msg_load_esc(const char *path);
/* このスレッド (回線) で ESC 版の文言を使うか */
void msg_set_esc(bool on);

#endif
