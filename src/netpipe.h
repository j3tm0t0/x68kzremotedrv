#ifndef _NETPIPE_H_
#define _NETPIPE_H_

#include <stdint.h>

/* SCSI ID 6 (DTYPE_REMOTECOMM) の中で netpipe が使う LBA */
#define NETPIPE_WLBA    0x1000          /* 要求を書く (8 セクタまで) */
#define NETPIPE_RLBA    0x2000          /* 応答を読む (0x2000～0x1ffff7 のどこでも、8 セクタ周期)。 */
#define NETPIPE_REND    0x200000        /* 読む位置は毎回変える: Z 側が読み込みをキャッシュする (帯は約 1GB)。
                                           0x200000 以降は scsiremote.sys が使う。ID6 は 2GB より大きくできない */

int netpipe_write(uint32_t lba, const uint8_t *buf);
int netpipe_read(uint32_t lba, uint8_t *buf);

#endif  /* _NETPIPE_H_ */
