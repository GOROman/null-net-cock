/*
 * XMODEM / YMODEM / ZMODEM によるファイルの送受信
 */
#ifndef XFER_H
#define XFER_H

#include "nc.h"

/* ホストが受信するときの方式 */
enum { XR_SUM, XR_CRC, XR_YMODEM, XR_YMODEM_G };
/* ホストが送信するときの方式 */
enum { XS_X128, XS_X1K, XS_YMODEM, XS_ZMODEM };

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

/* 受信。YMODEM / ZMODEM ならファイル名とサイズはヘッダから取る (name は XMODEM のときそのまま)。
 * どの方式で待っていても、相手が ZMODEM で送り始めたら ZMODEM で受け取る */
int xfer_recv(struct term *t, int proto, char *name, size_t namesz, unsigned char **data, size_t *len);
/* 送信。XMODEM は files[0] だけ、YMODEM / ZMODEM は n 個をバッチで。
 * XS_ZMODEM はホストから始める (端末ソフトの自動受信が動く)。ほかの方式でも、相手が ZMODEM の rz なら ZMODEM で送る */
int xfer_send(struct term *t, int proto, const struct xfile *files, int n);

/* ZMODEM (zmodem.c)。相手からの開始の合図は読み終えた状態で呼ぶ */
int zm_recv(struct term *t, char *name, size_t namesz, unsigned char **data, size_t *len);
int zm_send(struct term *t, const struct xfile *files, int n);

#endif
