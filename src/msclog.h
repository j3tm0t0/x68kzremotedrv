#ifndef _MSCLOG_H_
#define _MSCLOG_H_
#include <stdint.h>

/* MSC_LOG=1 (cmake -DMSC_LOG=ON) で、Z からの USB MSC の命令を 1 件ずつ UDP で送る (起動時の調査用)。既定は外す */
#ifndef MSC_LOG
#define MSC_LOG 0
#endif
#if MSC_LOG
void msclog_init(void);                                 /* 送信タスクを作る */
void msclog_rw(char dir, uint8_t lun, uint32_t lba, int rc, const uint8_t *buf);   /* read10/write10_cb から 1 セクタごとに */
void msclog_usb(const char *what);                      /* USB の mount / umount。記録の枠を戻す */
void msclog_mark(const char *fmt, ...);                 /* 出来事 (SMB 接続、HDS を開いた など) */
#else
#define msclog_init()           ((void)0)
#define msclog_rw(d, l, a, r, b) ((void)0)
#define msclog_usb(w)           ((void)0)
#define msclog_mark(...)        ((void)0)
#endif

#endif
