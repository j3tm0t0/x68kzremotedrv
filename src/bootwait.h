#ifndef _BOOTWAIT_H_
#define _BOOTWAIT_H_
#include <stdint.h>
#include <stdbool.h>
#include "bootwait_proto.h"

/* BOOTWAIT_SCREEN=1 (cmake -DBOOTWAIT_SCREEN=ON、BOOTWAIT と一緒に既定オン): HDS が開けるまで、控えておいた HDS の先頭と
   待ち IPL で起動を始め、Z の画面に接続待ちを出す (bootwait.c) */
#ifndef BOOTWAIT_SCREEN
#define BOOTWAIT_SCREEN 0
#endif

#define BW_FLASH_OFFSET 0x1e0000        /* 見出し 4KB + 中身 16KB (0x1e0000-0x1e4fff)。設定は 0x1f0000 */
#define BW_FLASH_SIZE   (4096 + BW_SNAP_SECTS * 512)

typedef int (*bw_read_fn)(void *ctx, uint32_t lba, uint8_t *buf);   /* HDS を 1 セクタ読む (0 なら成功) */

bool bw_usable(const char *path);                       /* 控えが壊れておらず、この HDS のものか */
int bw_disk_read(uint32_t lba, uint8_t *buf);           /* 控えから LBA 0-31 を返す (2 は待ち IPL の前半)。bw_usable のときだけ */
int bw_stage2_read(uint32_t lba, uint8_t *buf);         /* ID 6 の BW_STAGE2: 待ち IPL の後半 */
void bw_check(const char *path, uint64_t size, bw_read_fn rd, void *ctx);   /* HDS を開いたあと: 比べて、違えば控え直す */
void bw_reply(uint32_t lba, uint8_t *buf, int hds_open, int sysstat, uint32_t ip, uint32_t uptime_ms,
              uint32_t retry_ms, bw_read_fn rd, void *ctx); /* ID 6 の BW_BAND の読み込みに答える */
const char *bw_state(const char *path);                 /* status 用: none / stored / ok / updated / served / changed */

/* 書き込み (ホスト試験では差し替える) */
void bw_flash_erase(uint32_t off, uint32_t len);
void bw_flash_program(uint32_t off, const uint8_t *data, uint32_t len);
const uint8_t *bw_flash_ptr(void);

#endif
