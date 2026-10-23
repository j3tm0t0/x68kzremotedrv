/*
 * sdfs.c -- リモートドライブのユニットを SD カードの FatFs で答える (config の REMOTE の欄が "sd:/..." のユニット)
 *
 * remoteserv.c は全部の操作を fileop.h の FUNC_*(unit, ...) で呼ぶので、fileop.h がユニットの rootpath を見て
 * "sd:" ならここへ回す。パスは rootpath + Human68k のパス (UTF-8) で、先頭の "sd:" を外したものが FatFs のパス。
 * 戻り値は libsmb2 と同じ形 (失敗は -errno、ハンドルは 0 が失敗)。カードに触るところは sdc_lock の中。
 * FF_USE_CHMOD が 0 なので、ファイルの日付は変えられない (書いた時刻になる)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include "ff.h"
#include "smb2.h"
#include "libsmb2.h"
#include "sdhds.h"
#include "sdfs.h"

struct sdfile {
    FIL f;
    char path[];
};

struct sddir {
    DIR d;
    int dots;                           /* サブディレクトリでは "." と ".." を先に返す (FatFs は返さない) */
    FILINFO fi;
    struct smb2dirent de;
};

#define SDFS_LOG 0                      /* 1 で FatFs の失敗と書き込みの不足を UART に出す */
static int fr2errno_(FRESULT fr);
static int fr2errno_at(FRESULT fr, int line)
{
    if (SDFS_LOG && fr != FR_OK)
        printf("sdfs:%d fr=%d\n", line, fr);
    return fr2errno_(fr);
}
#define fr2errno(fr) fr2errno_at((fr), __LINE__)

static int fr2errno_(FRESULT fr)
{
    switch (fr) {
    case FR_OK:             return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:
    case FR_INVALID_NAME:   return ENOENT;
    case FR_EXIST:          return EEXIST;
    case FR_DENIED:         return EACCES;
    case FR_WRITE_PROTECTED: return EROFS;
    case FR_LOCKED:         return EACCES;  /* HDS として開いているファイルなど */
    case FR_TOO_MANY_OPEN_FILES: return EMFILE;
    case FR_NOT_ENOUGH_CORE: return ENOMEM;
    default:                return EIO;
    }
}

/* "sd:/a/b" -> "/a/b"。"." と ".." をたたむ (FatFs の f_stat は "/a/." を名前として扱えない) */
static void fpath(const char *path, char *out, size_t size)
{
    if (strncmp(path, "sd:", 3) == 0)
        path += 3;
    size_t n = 0;
    out[n++] = '/';
    while (*path) {
        while (*path == '/')
            path++;
        const char *e = strchr(path, '/');
        size_t len = e ? (size_t)(e - path) : strlen(path);
        if (len == 0)
            break;
        if (len == 1 && path[0] == '.') {
            /* 何もしない */
        } else if (len == 2 && path[0] == '.' && path[1] == '.') {
            while (n > 1 && out[n - 1] != '/')
                n--;
            if (n > 1)
                n--;
        } else if (n + (n > 1) + len < size) {
            if (n > 1)
                out[n++] = '/';
            memcpy(out + n, path, len);
            n += len;
        }
        path += len;
    }
    out[n] = 0;
}

static uint64_t fat2time(WORD fdate, WORD ftime)
{
    struct tm tm = { 0 };
    if (fdate == 0)
        fdate = (0 << 9) | (1 << 5) | 1;    /* 1980-01-01 */
    tm.tm_year = (fdate >> 9) + 80;
    tm.tm_mon = ((fdate >> 5) & 15) - 1;
    tm.tm_mday = fdate & 31;
    tm.tm_hour = ftime >> 11;
    tm.tm_min = (ftime >> 5) & 63;
    tm.tm_sec = (ftime & 31) * 2;
    tm.tm_isdst = 0;
    return (uint64_t)mktime(&tm);
}

static void fi2stat(const FILINFO *fi, struct smb2_stat_64 *st)
{
    memset(st, 0, sizeof *st);
    st->smb2_type = (fi->fattrib & AM_DIR) ? SMB2_TYPE_DIRECTORY : SMB2_TYPE_FILE;
    st->smb2_size = fi->fsize;
    st->smb2_mtime = fat2time(fi->fdate, fi->ftime);
}

static FRESULT stat_locked(const char *p, struct smb2_stat_64 *st)
{
    FILINFO fi;
    if (strcmp(p, "/") == 0) {          /* ルートは f_stat で引けない */
        memset(&fi, 0, sizeof fi);
        fi.fattrib = AM_DIR;
        fi2stat(&fi, st);
        return FR_OK;
    }
    FRESULT fr = f_stat(p, &fi);
    if (fr == FR_OK)
        fi2stat(&fi, st);
    return fr;
}

bool sdfs_is(const char *rootpath)
{
    return rootpath != NULL && sdhds_is(rootpath);
}

int sdfs_stat(const char *path, struct smb2_stat_64 *st)
{
    char p[256];
    if (!sdc_mount())
        return -EIO;
    fpath(path, p, sizeof p);
    sdc_lock();
    FRESULT fr = stat_locked(p, st);
    sdc_unlock();
    return -fr2errno(fr);
}

int sdfs_mkdir(const char *path)
{
    char p[256];
    fpath(path, p, sizeof p);
    sdc_lock();
    FRESULT fr = f_mkdir(p);
    sdc_unlock();
    return -fr2errno(fr);
}

int sdfs_rmdir(const char *path)
{
    char p[256];
    FILINFO fi;
    fpath(path, p, sizeof p);
    sdc_lock();
    FRESULT fr = f_stat(p, &fi);
    int e = fr2errno(fr);
    if (fr == FR_OK) {
        if (!(fi.fattrib & AM_DIR))
            e = ENOTDIR;
        else if ((fr = f_unlink(p)) != FR_OK)
            e = fr == FR_DENIED ? ENOTEMPTY : fr2errno(fr);
    }
    sdc_unlock();
    return -e;
}

int sdfs_rename(const char *pathold, const char *pathnew)
{
    char a[256], b[256];
    fpath(pathold, a, sizeof a);
    fpath(pathnew, b, sizeof b);
    sdc_lock();
    FRESULT fr = f_rename(a, b);
    sdc_unlock();
    return -fr2errno(fr);
}

int sdfs_unlink(const char *path)
{
    char p[256];
    FILINFO fi;
    fpath(path, p, sizeof p);
    sdc_lock();
    FRESULT fr = f_stat(p, &fi);
    int e = fr2errno(fr);
    if (fr == FR_OK)
        e = (fi.fattrib & AM_DIR) ? EISDIR : fr2errno(f_unlink(p));     /* f_unlink は空のディレクトリも消すので先に見る */
    sdc_unlock();
    return -e;
}

uint64_t sdfs_opendir(const char *path, int *err)
{
    char p[256];
    struct sddir *d;
    if (!sdc_mount()) {
        *err = EIO;
        return 0;
    }
    if ((d = calloc(1, sizeof *d)) == NULL) {
        *err = ENOMEM;
        return 0;
    }
    fpath(path, p, sizeof p);
    sdc_lock();
    FRESULT fr = f_opendir(&d->d, p);
    sdc_unlock();
    *err = fr2errno(fr);
    if (fr != FR_OK) {
        free(d);
        return 0;
    }
    d->dots = strcmp(p, "/") == 0 ? 2 : 0;
    return SDFS_HANDLE(d);
}

struct smb2dirent *sdfs_readdir(uint64_t dir)
{
    struct sddir *d = (struct sddir *)(uintptr_t)(uint32_t)dir;
    if (d->dots < 2) {
        d->de.name = d->dots++ == 0 ? "." : "..";
        memset(&d->de.st, 0, sizeof d->de.st);
        d->de.st.smb2_type = SMB2_TYPE_DIRECTORY;
        return &d->de;
    }
    sdc_lock();
    FRESULT fr = f_readdir(&d->d, &d->fi);
    sdc_unlock();
    if (fr != FR_OK || d->fi.fname[0] == 0)
        return NULL;
    d->de.name = d->fi.fname;
    fi2stat(&d->fi, &d->de.st);
    return &d->de;
}

int sdfs_closedir(uint64_t dir)
{
    struct sddir *d = (struct sddir *)(uintptr_t)(uint32_t)dir;
    sdc_lock();
    f_closedir(&d->d);
    sdc_unlock();
    free(d);
    return 0;
}

uint64_t sdfs_open(const char *path, int flags, int *err)
{
    char p[256];
    BYTE mode;
    if (!sdc_mount()) {
        *err = EIO;
        return 0;
    }
    fpath(path, p, sizeof p);
    switch (flags & O_ACCMODE) {
    case O_RDONLY:  mode = FA_READ; break;
    case O_WRONLY:  mode = FA_WRITE; break;
    default:        mode = FA_READ | FA_WRITE; break;
    }
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
        mode |= FA_CREATE_NEW;
    else if ((flags & (O_CREAT | O_TRUNC)) == (O_CREAT | O_TRUNC))
        mode |= FA_CREATE_ALWAYS;
    else if (flags & O_CREAT)
        mode |= FA_OPEN_ALWAYS;
    struct sdfile *f = malloc(sizeof *f + strlen(p) + 1);
    if (f == NULL) {
        *err = ENOMEM;
        return 0;
    }
    strcpy(f->path, p);
    sdc_lock();
    FRESULT fr = f_open(&f->f, p, mode);
    if (fr == FR_OK && (flags & O_TRUNC) && !(mode & FA_CREATE_ALWAYS))
        fr = f_truncate(&f->f);
    if (fr == FR_NO_FILE) {
        FILINFO fi;
        if (f_stat(p, &fi) == FR_OK && (fi.fattrib & AM_DIR)) {
            sdc_unlock();
            free(f);
            *err = EISDIR;
            return 0;
        }
    }
    sdc_unlock();
    *err = fr2errno(fr);
    if (fr != FR_OK) {
        free(f);
        return 0;
    }
    return SDFS_HANDLE(f);
}

int sdfs_close(uint64_t fd)
{
    struct sdfile *f = (struct sdfile *)(uintptr_t)(uint32_t)fd;
    sdc_lock();
    FRESULT fr = f_close(&f->f);
    sdc_unlock();
    free(f);
    return -fr2errno(fr);
}

ssize_t sdfs_read(uint64_t fd, void *buf, size_t count)
{
    struct sdfile *f = (struct sdfile *)(uintptr_t)(uint32_t)fd;
    UINT br;
    sdc_lock();
    FRESULT fr = f_read(&f->f, buf, count, &br);
    sdc_unlock();
    return fr == FR_OK ? (ssize_t)br : -fr2errno(fr);
}

ssize_t sdfs_write(uint64_t fd, const void *buf, size_t count)
{
    struct sdfile *f = (struct sdfile *)(uintptr_t)(uint32_t)fd;
    UINT bw;
    sdc_lock();
    FRESULT fr = f_write(&f->f, buf, count, &bw);
    sdc_unlock();
    if (SDFS_LOG && (fr != FR_OK || bw != count))
        printf("sdfs: write %u -> fr=%d bw=%u\n", (unsigned)count, fr, (unsigned)bw);
    if (fr != FR_OK)
        return -fr2errno(fr);
    return bw < count ? (bw > 0 ? (ssize_t)bw : -ENOSPC) : (ssize_t)bw;
}

int sdfs_ftruncate(uint64_t fd, uint64_t length)
{
    struct sdfile *f = (struct sdfile *)(uintptr_t)(uint32_t)fd;
    sdc_lock();
    FSIZE_t pos = f_tell(&f->f);
    FRESULT fr = f_lseek(&f->f, length);    /* 長くするときは f_lseek が伸ばす */
    if (fr == FR_OK)
        fr = f_truncate(&f->f);
    if (fr == FR_OK && pos < length)
        fr = f_lseek(&f->f, pos);
    sdc_unlock();
    return -fr2errno(fr);
}

int64_t sdfs_lseek(uint64_t fd, int64_t offset, int whence)
{
    struct sdfile *f = (struct sdfile *)(uintptr_t)(uint32_t)fd;
    sdc_lock();
    int64_t pos = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (int64_t)f_tell(&f->f) + offset : (int64_t)f_size(&f->f) + offset;
    FRESULT fr = FR_OK;
    if (pos < 0)
        fr = FR_INVALID_PARAMETER;
    else
        fr = f_lseek(&f->f, pos);       /* 書き込みで開いたファイルは末尾の先まで伸びる (op_write が pos に書けるように。読み込みだけなら末尾で止まる) */
    pos = f_tell(&f->f);
    sdc_unlock();
    return fr == FR_OK ? pos : -EINVAL;
}

int sdfs_fstat(uint64_t fd, struct smb2_stat_64 *st)
{
    struct sdfile *f = (struct sdfile *)(uintptr_t)(uint32_t)fd;
    sdc_lock();
    FRESULT fr = f_sync(&f->f);         /* 書いた分の大きさと時刻をディレクトリに出してから引く */
    if (fr == FR_OK || fr == FR_DENIED)
        fr = stat_locked(f->path, st);
    if (fr == FR_OK)
        st->smb2_size = f_size(&f->f);
    sdc_unlock();
    return -fr2errno(fr);
}

int sdfs_statfs(const char *path, uint64_t *total, uint64_t *avail)
{
    FATFS *fs;
    DWORD nclst;
    *total = *avail = 0;
    if (!sdc_mount())
        return -EIO;
    sdc_lock();
    FRESULT fr = f_getfree("", &nclst, &fs);
    if (fr == FR_OK) {
        *total = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512;
        *avail = (uint64_t)nclst * fs->csize * 512;
    }
    sdc_unlock();
    return -fr2errno(fr);
}
