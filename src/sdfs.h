#ifndef _SDFS_H_
#define _SDFS_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

/* リモートドライブのユニットのうち rootpath が "sd:" のものを SD カードで答える (fileop.h から) */
struct smb2_stat_64;
struct smb2dirent;
bool sdfs_is(const char *rootpath);
/* ファイルとディレクトリのハンドルは上位 32 ビットに印を付ける (SMB のハンドルは上位が smb2_context のポインタ)。
   Z のドライバは READ / WRITE などファイル番号の操作ではユニット番号を送ってこない (0 になる) ので、
   ハンドルの操作はユニットではなくこの印で振り分ける */
#define SDFS_TAG            0xfffe5344u
#define SDFS_HANDLE(p)      (((uint64_t)SDFS_TAG << 32) | (uint32_t)(uintptr_t)(p))
#define SDFS_IS_HANDLE(h)   ((uint32_t)((h) >> 32) == SDFS_TAG)
int sdfs_stat(const char *path, struct smb2_stat_64 *st);
int sdfs_mkdir(const char *path);
int sdfs_rmdir(const char *path);
int sdfs_rename(const char *pathold, const char *pathnew);
int sdfs_unlink(const char *path);
uint64_t sdfs_opendir(const char *path, int *err);
struct smb2dirent *sdfs_readdir(uint64_t dir);
int sdfs_closedir(uint64_t dir);
uint64_t sdfs_open(const char *path, int flags, int *err);
int sdfs_close(uint64_t fd);
ssize_t sdfs_read(uint64_t fd, void *buf, size_t count);
ssize_t sdfs_write(uint64_t fd, const void *buf, size_t count);
int sdfs_ftruncate(uint64_t fd, uint64_t length);
int64_t sdfs_lseek(uint64_t fd, int64_t offset, int whence);
int sdfs_fstat(uint64_t fd, struct smb2_stat_64 *st);
int sdfs_statfs(const char *path, uint64_t *total, uint64_t *avail);

#endif  /* _SDFS_H_ */
