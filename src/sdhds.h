#ifndef _SDHDS_H_
#define _SDHDS_H_

#include <stdint.h>
#include <stdbool.h>

/* config の HDS の指定が "sd:" で始まれば、Pico に SPI でつないだ SD カードの上のファイルを使う */
bool sdhds_is(const char *path);
/* SD カードの上のデータ用ディスクの置き場の既定。使うには HDS1 に "sd:/x68000z/hds1.hds" と明示する
   (SCSI ID 1 が起動時に B: を取るので、リモートドライブは C: にずれる) */
#define SDHDS_AUTO1     "/x68000z/hds1.hds"
/* SD に SDHDS_AUTO1 が無く、SMB の共有 (HDS0 と同じ共有) にこれがあれば、起動時に 1 回だけカードへ写す
   (写すだけで、HDS1 に指定しない限りディスクにはならない) */
#define SDHDS_FETCH1    "sdcopy/hds1.hds"
struct smb2_context;
/* カードに触るところは全部 sdc_lock の中で (FatFs は 1 本の錠で守る)。sdc_mount は最初の 1 回だけ実際に mount する */
void sdc_init(void);              /* vd_init の最初 (スケジューラの前) */
void sdc_lock(void);
void sdc_unlock(void);
bool sdc_mount(void);
/* sd_path ("/..." か "sd:/...") を HDS として開いているか */
bool sdhds_in_use(const char *sdpath);
int64_t sdhds_fetch(struct smb2_context *smb2, const char *smbpath, const char *sdpath);
/* 開いて大きさ (バイト) を返す。失敗なら -1。戻り値の handle を hds_cache_* に smb2 = NULL で渡す */
int64_t sdhds_open(const char *path, void **handle);
int sdhds_read(void *handle, uint32_t lba, uint8_t *buf, int nsect);
int sdhds_write(void *handle, uint32_t lba, const uint8_t *buf, int nsect);

/* UDP 6869 の sdcopy <共有名/パス> <sd-path> (hidkbd.c から) */
void sdcopy_init(void);
const char *sdcopy_request(const char *args);
int sdcopy_status(char *s, size_t size);

#endif  /* _SDHDS_H_ */
