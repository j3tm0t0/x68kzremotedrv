#ifndef _NETPIPE_H_
#define _NETPIPE_H_

#include <stdint.h>

/* SCSI ID 6 (DTYPE_REMOTECOMM) の中で netpipe が使う LBA */
#define NETPIPE_WLBA    0x100000        /* 要求を書く (8 セクタまで) */
#define NETPIPE_RLBA    0x180000        /* 応答を読む (0x180000～0x1fffff のどこでも、8 セクタ周期) */
#define NETPIPE_REND    0x200000

int netpipe_write(uint32_t lba, const uint8_t *buf);
int netpipe_read(uint32_t lba, uint8_t *buf);

#endif  /* _NETPIPE_H_ */
