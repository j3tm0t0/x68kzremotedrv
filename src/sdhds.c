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
#include <stdlib.h>
#include <strings.h>
#include <fcntl.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#if SD_CARD
#include "pico/aon_timer.h"
#include "ff.h"
#endif
#include "smb2.h"
#include "libsmb2.h"
#include "main.h"
#include "vd_command.h"
#include "sdhds.h"

#if SD_CARD
#define SDHDS_MAX   6

static FATFS fs;
static bool mounted;
static FIL *files[SDHDS_MAX];           /* 開くときに確保 (Pico W の RAM: 使わなければ 0) */
static char names[SDHDS_MAX][64];       /* HDS として開いているファイル (sdcopy が上書きしないように) */
static int nfiles;

/* FatFs は FF_FS_REENTRANT 0 なので、カードに触るところは全部この 1 本の錠で囲む
   (USB タスクの HDS とリモートドライブ、接続タスクの fetch、sdcopy タスク) */
static SemaphoreHandle_t sd_mtx;

void sdc_init(void)
{
    /* FatFs の lib (my_rtc.c) が time() を always-on タイマーで置き換えていて、止まっていると 0 のまま進まない
       (libsmb2 の待ちの時間切れが効かない)。起動時に 0 から動かしておき、SMB の時刻が取れたら connect.c で合わせる */
    if (!aon_timer_is_running()) {
        struct timespec ts = { 0, 0 };
        aon_timer_start(&ts);
    }
    if (sd_mtx == NULL)
        sd_mtx = xSemaphoreCreateRecursiveMutex();
}

void sdc_lock(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
        return;                         /* vd_init はスケジューラの前 (ほかのタスクはまだいない) */
    xSemaphoreTakeRecursive(sd_mtx, portMAX_DELAY);
}

void sdc_unlock(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
        return;
    xSemaphoreGiveRecursive(sd_mtx);
}

bool sdc_mount(void)
{
    static bool failed;
    FRESULT fr;
    if (mounted)
        return true;
    if (failed)
        return false;                   /* カードが無いとき、毎回 SPI で待たない */
    sdc_lock();
    if (!mounted) {
        if ((fr = f_mount(&fs, "", 1)) == FR_OK)
            mounted = true;
        else {
            printf("SD: mount failed (%d)\n", fr);
            failed = true;
        }
    }
    sdc_unlock();
    return mounted;
}

bool sdhds_is(const char *path)
{
    return strncmp(path, "sd:", 3) == 0;
}

int64_t sdhds_open(const char *path, void **handle)
{
    FRESULT fr;
    int64_t sz = -1;
    if (!sdc_mount())
        return -1;
    sdc_lock();
    FIL *f;
    if (nfiles < SDHDS_MAX && (f = malloc(sizeof *f)) != NULL) {
        const char *name = path + 3;        /* "sd:" の後ろ */
        if ((fr = f_open(f, name, FA_READ | FA_WRITE)) != FR_OK) {
            printf("SD: %s open failed (%d)\n", name, fr);
            free(f);
        } else {
            files[nfiles] = f;
            snprintf(names[nfiles], sizeof names[0], "%s", name);
            nfiles++;
            *handle = f;
            sz = (int64_t)f_size(f);
        }
    }
    sdc_unlock();
    return sz;
}

bool sdhds_in_use(const char *sdpath)
{
    if (strncmp(sdpath, "sd:", 3) == 0)
        sdpath += 3;
    for (int i = 0; i < nfiles; i++)
        if (strcasecmp(names[i], sdpath) == 0)
            return true;
    return false;
}

int sdhds_read(void *handle, uint32_t lba, uint8_t *buf, int nsect)
{
    FIL *f = handle;
    UINT br;
    int r = -1;
    sdc_lock();
    if (f_lseek(f, (FSIZE_t)lba * 512) == FR_OK && f_read(f, buf, nsect * 512, &br) == FR_OK)
        r = br;
    sdc_unlock();
    return r;
}

int sdhds_write(void *handle, uint32_t lba, const uint8_t *buf, int nsect)
{
    FIL *f = handle;
    UINT bw;
    int r = -1;
    sdc_lock();
    if (f_lseek(f, (FSIZE_t)lba * 512) == FR_OK &&
        f_write(f, buf, nsect * 512, &bw) == FR_OK && bw == (UINT)nsect * 512 &&
        f_sync(f) == FR_OK)
        r = bw;
    sdc_unlock();
    return r;
}

int sdc_read_file(const char *path, char *buf, int max)
{
    FIL f;
    UINT br;
    int r = -1;
    if (!sdc_mount())
        return -1;
    sdc_lock();
    if (f_open(&f, path, FA_READ) == FR_OK) {
        if (f_read(&f, buf, max - 1, &br) == FR_OK) {
            buf[br] = '\0';
            r = br;
        }
        f_close(&f);
    }
    sdc_unlock();
    return r;
}

#else   /* !SD_CARD: SD カードの無いビルド。呼ぶ側を変えずに済むように何もしない版 */

void sdc_init(void) {}
void sdc_lock(void) {}
void sdc_unlock(void) {}
bool sdc_mount(void) { return false; }
bool sdhds_is(const char *path) { return strncmp(path, "sd:", 3) == 0; }
int64_t sdhds_open(const char *path, void **handle) { (void)path; (void)handle; printf("SD: not in this build\n"); return -1; }
bool sdhds_in_use(const char *sdpath) { (void)sdpath; return false; }
int sdhds_read(void *handle, uint32_t lba, uint8_t *buf, int nsect) { return -1; }
int sdhds_write(void *handle, uint32_t lba, const uint8_t *buf, int nsect) { return -1; }
int sdc_read_file(const char *path, char *buf, int max) { return -1; }
#endif  /* SD_CARD */

#if SD_CARD && !PICO_RP2040
/*
 * SMB の共有から SD カードへ写す (Wi-Fi 越しにカードへ HDS を載せる道。カードを抜かずに済む)。
 * 途中で切れたものを使わないよう、別名 (.part) に書いてから名前を変える。上書きは HDS として開いていないファイルだけ。
 * 1 塊ずつ SMB から読み (錠の外)、カードに書く (錠の中)。USB タスクの HDS の読み書きは塊の合間に入る。
 * 戻り値: 写したバイト数 / -1 (失敗、*why に理由)
 */
static volatile uint64_t cp_done, cp_total;

static int64_t sd_copy(struct smb2_context *smb2, const char *smbpath, const char *sdpath, const char **why)
{
    size_t bsize = 32768;
    uint8_t *buf = malloc(bsize);
    char tmp[128];
    FIL *f = NULL;
    FRESULT fr;
    struct smb2fh *fh = NULL;
    struct smb2_stat_64 st;
    int64_t total = -1;

    if (buf == NULL && (buf = malloc(bsize = 4096)) == NULL) {
        *why = "nomem";
        return -1;
    }
    if ((f = malloc(sizeof *f)) == NULL) {
        *why = "nomem";
        goto out;
    }
    if ((fh = smb2_open(smb2, smbpath, O_RDONLY)) == NULL) {
        *why = "smb-open";
        goto out;
    }
    cp_total = smb2_fstat(smb2, fh, &st) == 0 ? st.smb2_size : 0;
    cp_done = 0;
    snprintf(tmp, sizeof tmp, "%s.part", sdpath);
    sdc_lock();
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
    fr = f_open(f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    sdc_unlock();
    if (fr != FR_OK) {
        printf("SD: copy %s: create failed (%d)\n", tmp, fr);
        *why = "sd-create";
        goto out;
    }
    printf("SD: copying %s -> %s (%llu bytes)\n", smbpath, sdpath, (unsigned long long)cp_total);
    total = 0;
    for (;;) {
        int n = smb2_read(smb2, fh, buf, bsize);
        UINT bw;
        if (n < 0) {
            *why = "smb-read";
            total = -1;
            break;
        }
        if (n == 0)
            break;
        sdc_lock();
        fr = f_write(f, buf, n, &bw);
        sdc_unlock();
        if (fr != FR_OK || bw != (UINT)n) {
            *why = fr == FR_OK ? "sd-full" : "sd-write";
            total = -1;
            break;
        }
        total += n;
        cp_done = total;
    }
    sdc_lock();
    if (f_close(f) != FR_OK && total >= 0) {
        *why = "sd-close";
        total = -1;
    }
    if (total >= 0) {
        if (sdhds_in_use(sdpath)) {
            *why = "in-use";            /* 写している間に HDS として開かれた */
            total = -1;
        } else if ((fr = f_unlink(sdpath)) != FR_OK && fr != FR_NO_FILE) {
            *why = fr == FR_LOCKED ? "in-use" : "sd-unlink";
            total = -1;
        } else if (f_rename(tmp, sdpath) != FR_OK) {
            *why = "sd-rename";
            total = -1;
        }
    }
    if (total < 0)
        f_unlink(tmp);
    sdc_unlock();
    if (total < 0)
        printf("SD: copy %s failed (%s)\n", smbpath, *why);
    else
        printf("SD: copied %lld bytes to %s\n", total, sdpath);
out:
    if (fh)
        smb2_close(smb2, fh);
    free(f);
    free(buf);
    return total;
}

/* 起動時に 1 回: SD に sdpath が無く、共有に smbpath があるときだけ写す。戻り値: バイト数 / 0 (写すものが無い・もうある) / -1 */
int64_t sdhds_fetch(struct smb2_context *smb2, const char *smbpath, const char *sdpath)
{
    FILINFO fi;
    FRESULT fr;
    struct smb2_stat_64 st;
    const char *why;
    if (!sdc_mount())
        return -1;
    sdc_lock();
    fr = f_stat(sdpath, &fi);
    sdc_unlock();
    if (fr == FR_OK || smb2_stat(smb2, smbpath, &st) < 0)
        return 0;
    return sd_copy(smb2, smbpath, sdpath, &why);
}

/*
 * sdcopy <smb-path> <sd-path>: UDP 6869 から。<smb-path> は config の HDS と同じ「共有名/パス」、<sd-path> は "/..." か "sd:/..."。
 * 専用のタスクが別の SMB の接続で写す (USB タスクの接続とは分ける)。様子は status の sdcopy=... に出る。
 */
static char cp_src[256], cp_dst[128];
static volatile enum { CP_IDLE, CP_RUN, CP_DONE, CP_ERR } cp_state;
static const char *cp_why = "";

/* 写すたびに作り、終わったら消す (待っている間のスタック 8KB を Pico W で持たないため) */
static void sdcopy_task(void *params)
{
    {
        const char *why = "smb-connect";
        int64_t r = -1;
        char share[64];
        const char *slash = strchr(cp_src, '/');
        if (sysstatus < STAT_SMB2_CONNECTED) {
            why = "no-smb";
        } else if (!slash || slash == cp_src || (size_t)(slash - cp_src) >= sizeof share) {
            why = "bad-path";
        } else if (!sdc_mount()) {
            why = "no-card";
        } else {
            memcpy(share, cp_src, slash - cp_src);
            share[slash - cp_src] = 0;
            struct smb2_context *smb2 = connect_smb2(share);
            if (smb2 != NULL) {
                r = sd_copy(smb2, slash + 1, cp_dst, &why);
                disconnect_smb2(smb2);
            }
        }
        cp_why = why;
        __sync_synchronize();
        cp_state = r < 0 ? CP_ERR : CP_DONE;
    }
    vTaskDelete(NULL);
}

void sdcopy_init(void)
{
}

/* lwIP のスレッドから。args は "sdcopy " の後ろ。返事の文字列を返す */
const char *sdcopy_request(const char *args)
{
    char src[sizeof cp_src], dst[sizeof cp_dst];
    if (cp_state == CP_RUN)
        return "err sdcopy busy";             /* 前のタスクは CP_RUN を下ろしてから自分を消す */
    if (sscanf(args, "%255s %127s", src, dst) != 2)
        return "err usage: sdcopy <share/path> <sd-path>";
    const char *d = strncmp(dst, "sd:", 3) == 0 ? dst + 3 : dst;
    if (d[0] != '/' || strlen(d) + 6 > sizeof cp_dst)
        return "err sd-path must start with / (or sd:/)";
    if (sdhds_in_use(d))
        return "err in-use (open as an HDS; the Z is using that SCSI ID)";
    strcpy(cp_src, src);
    strcpy(cp_dst, d);
    cp_done = cp_total = 0;
    __sync_synchronize();
    cp_state = CP_RUN;
    if (xTaskCreate(sdcopy_task, "SdCopy", 2048, NULL, 1, NULL) != pdPASS) {
        cp_why = "nomem";
        cp_state = CP_ERR;
        return "err sdcopy nomem";
    }
    return "ok sdcopy started";
}

int sdcopy_status(char *s, size_t size)
{
    switch (cp_state) {
    case CP_IDLE:
        return snprintf(s, size, " sdcopy=idle");
    case CP_RUN:
        return snprintf(s, size, " sdcopy=run:%llu/%llu", (unsigned long long)cp_done, (unsigned long long)cp_total);
    case CP_DONE:
        return snprintf(s, size, " sdcopy=done:%llu", (unsigned long long)cp_done);
    default:
        return snprintf(s, size, " sdcopy=err:%s", cp_why);
    }
}
#else   /* SD カードが無いか Pico W (RP2040): 写す機能は無い (RAM が足りない) */
int64_t sdhds_fetch(struct smb2_context *smb2, const char *smbpath, const char *sdpath) { return 0; }
void sdcopy_init(void) {}
const char *sdcopy_request(const char *args) { (void)args; return "err sdcopy not in this build"; }
int sdcopy_status(char *s, size_t size) { (void)s; (void)size; return 0; }
#endif
