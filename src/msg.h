#ifndef MSG_H
#define MSG_H

/* ID で文言を引く (ID の割り当ては NET-COCK の MES.TXT と同じ) */
const char *M(int id);
/* NET-COCK の MES.TXT を読み込む。読めたメッセージ数を返す */
int msg_load(const char *path);
/* NET-COCK の SYS_MES.DAT を読み込む */
int sysmes_load(const char *path);

#endif
