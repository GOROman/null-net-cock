/*
 * XMODEM / YMODEM によるファイルの送受信
 */
#ifndef XFER_H
#define XFER_H

#include "nc.h"

/* ホストが受信するときの方式 */
enum { XR_SUM, XR_CRC, XR_YMODEM, XR_YMODEM_G };
/* ホストが送信するときの方式 */
enum { XS_X128, XS_X1K, XS_YMODEM };

/* 結果: 0 成功 / XF_CANCEL 相手が中止 / XF_FAIL エラーが続いた / 負は切断 */
#define XF_CANCEL 1
#define XF_FAIL 2
#define XF_BADNAME 3

#define XFER_MAX_SIZE (8L * 1024 * 1024)

struct xfile {
    const char *name;
    const unsigned char *data;
    size_t len;
    time_t mtime;
};

/* 受信。YMODEM ならファイル名とサイズはヘッダから取る (name は XMODEM のときそのまま) */
int xfer_recv(struct term *t, int proto, char *name, size_t namesz, unsigned char **data, size_t *len);
/* 送信。XMODEM は files[0] だけ、YMODEM は n 個をバッチで */
int xfer_send(struct term *t, int proto, const struct xfile *files, int n);

#endif
