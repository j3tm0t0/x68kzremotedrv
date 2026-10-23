/*
 * sdhds.c -- SD カードの上の HDS ファイル (FatFs、no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
 *
 * Wi-Fi の SMB2 を待たずに起動できる (電源を入れた直後の Wi-Fi / SMB2 の接続が間に合わないと
 * 「ディスクから起動できません」になっていた)。B: (SCSI ID 6) と netpipe は Wi-Fi がつながってから使える。
 * HDS の大きさは変えない (書き込みは既存の範囲の中だけ) ので、FAT やディレクトリは書き換わらない。
 * それでも FatFs の中の 1 セクタ分の書き込みを溜めないよう、書いたら f_sync する
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>

#include "ff.h"
#include "smb2.h"
#include "libsmb2.h"
#include "sdhds.h"

#define SDHDS_MAX   6

static FATFS fs;
static bool mounted;
static FIL files[SDHDS_MAX];
static int nfiles;

bool sdhds_is(const char *path)
{
    return strncmp(path, "sd:", 3) == 0;
}

int64_t sdhds_open(const char *path, void **handle)
{
    FRESULT fr;
    if (!mounted) {
        if ((fr = f_mount(&fs, "", 1)) != FR_OK) {
            printf("SD: mount failed (%d)\n", fr);
            return -1;
        }
        mounted = true;
    }
    if (nfiles >= SDHDS_MAX)
        return -1;
    FIL *f = &files[nfiles];
    const char *name = path + 3;            /* "sd:" の後ろ */
    if ((fr = f_open(f, name, FA_READ | FA_WRITE)) != FR_OK) {
        printf("SD: %s open failed (%d)\n", name, fr);
        return -1;
    }
    nfiles++;
    *handle = f;
    return (int64_t)f_size(f);
}

int sdhds_read(void *handle, uint32_t lba, uint8_t *buf, int nsect)
{
    FIL *f = handle;
    UINT br;
    if (f_lseek(f, (FSIZE_t)lba * 512) != FR_OK)
        return -1;
    if (f_read(f, buf, nsect * 512, &br) != FR_OK)
        return -1;
    return br;
}

int sdhds_write(void *handle, uint32_t lba, const uint8_t *buf, int nsect)
{
    FIL *f = handle;
    UINT bw;
    if (f_lseek(f, (FSIZE_t)lba * 512) != FR_OK)
        return -1;
    if (f_write(f, buf, nsect * 512, &bw) != FR_OK || bw != (UINT)nsect * 512)
        return -1;
    if (f_sync(f) != FR_OK)
        return -1;
    return bw;
}

/*
 * SMB の共有から SD カードへ 1 回だけ写す (Wi-Fi 越しにカードへ HDS を載せる道。カードを抜かずに済む)。
 * SD に sdpath が無く、共有に smbpath があるときだけ。途中で切れたものを使わないよう、別名に書いてから名前を変える。
 * 戻り値: 写したバイト数 / 0 (写すものが無い・もうある) / -1 (失敗)
 */
int64_t sdhds_fetch(struct smb2_context *smb2, const char *smbpath, const char *sdpath)
{
    static uint8_t buf[32768];
    char tmp[128];
    FILINFO fi;
    FIL f;
    FRESULT fr;
    struct smb2fh *fh;
    int64_t total = 0;
    if (!mounted) {
        if ((fr = f_mount(&fs, "", 1)) != FR_OK)
            return -1;
        mounted = true;
    }
    if (f_stat(sdpath, &fi) == FR_OK)
        return 0;
    if ((fh = smb2_open(smb2, smbpath, O_RDONLY)) == NULL)
        return 0;
    snprintf(tmp, sizeof tmp, "%s.part", sdpath);
    {
        /* 親ディレクトリ (1 段だけ) */
        char d[128];
        snprintf(d, sizeof d, "%s", sdpath);
        char *p = strrchr(d, '/');
        if (p && p != d) {
            *p = 0;
            f_mkdir(d);
        }
    }
    if ((fr = f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK) {
        printf("SD: fetch %s: create failed (%d)\n", tmp, fr);
        smb2_close(smb2, fh);
        return -1;
    }
    printf("SD: fetching %s -> %s\n", smbpath, sdpath);
    for (;;) {
        int n = smb2_read(smb2, fh, buf, sizeof buf);
        UINT bw;
        if (n < 0) {
            total = -1;
            break;
        }
        if (n == 0)
            break;
        if (f_write(&f, buf, n, &bw) != FR_OK || bw != (UINT)n) {
            total = -1;
            break;
        }
        total += n;
    }
    smb2_close(smb2, fh);
    if (f_close(&f) != FR_OK)
        total = -1;
    if (total < 0 || f_rename(tmp, sdpath) != FR_OK) {
        printf("SD: fetch %s failed\n", smbpath);
        f_unlink(tmp);
        return -1;
    }
    printf("SD: fetched %lld bytes to %s (used from the next boot)\n", total, sdpath);
    return total;
}
