/* 
 * Copyright (c) 2024,2025 Yuichi Nakamura (@yunkya2)
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>

#include "pico/cyw43_arch.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "smb2.h"
#include "libsmb2.h"
#include "bsp/board_api.h"
#include "tusb.h"

#include "config.h"
#include "main.h"
#include "sdhds.h"
#if SD_CARD
#include "pico/aon_timer.h"
#endif
#include "pico/unique_id.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "virtual_disk.h"
#include "config_file.h"
#include "msclog.h"
#include "bootwait.h"

//****************************************************************************
// Global variables
//****************************************************************************

uint64_t boottime = 0;

volatile int sysstatus = STAT_WIFI_DISCONNECTED;
#if BOOTWAIT_SCREEN
volatile uint32_t connect_next_ms;  /* 次に繋ぎにいく時刻 (0: 試している最中、UINT32_MAX: 予定なし)。待ち IPL に見せる */
#define NEXT_IN(ms) (connect_next_ms = to_ms_since_boot(get_absolute_time()) + (ms))
#define NEXT_SET(v) (connect_next_ms = (v))
#else
#define NEXT_IN(ms) ((void)0)
#define NEXT_SET(v) ((void)0)
#endif
#if HID_KEYBOARD
volatile bool connect_finished;     /* 起動時の接続 (繰り直しを含む) が終わった。hidkbd の status 用 */
#endif

const char *rootpath[8];

struct diskinfo diskinfo[7];

//****************************************************************************
// Static variables
//****************************************************************************

//****************************************************************************
// Private functions
//****************************************************************************

/* ホスト名 (DHCP で名乗る。どの基板も "PicoW" だったので見分けが付かなかった) と固定 IP。Wi-Fi につなぐ前に 1 回 */
static char hostname[32];

const char *net_hostname(void)
{
    return hostname;
}

void net_setup(void)
{
    if (config.hostname[0] != '\0') {
        snprintf(hostname, sizeof hostname, "%s", config.hostname);
    } else {
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        snprintf(hostname, sizeof hostname, "pico-%02x%02x",
                 id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2], id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
    }
    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    ip4_addr_t ip, mask, gw, dns;
    cyw43_arch_lwip_begin();
    netif_set_hostname(n, hostname);
    if (config.ipaddr[0] != '\0' && ip4addr_aton(config.ipaddr, &ip)) {
        /* 固定 IP: DHCP を止めて番地を決める (つなぐ前なので DHCP はまだ何も送っていない) */
        if (config.netmask[0] == '\0' || !ip4addr_aton(config.netmask, &mask))
            IP4_ADDR(&mask, 255, 255, 255, 0);
        if (config.gateway[0] == '\0' || !ip4addr_aton(config.gateway, &gw))
            ip4_addr_set_zero(&gw);
        if (config.dns[0] == '\0' || !ip4addr_aton(config.dns, &dns))
            dns = gw;
        dhcp_stop(n);
        netif_set_addr(n, &ip, &mask, &gw);
        ip_addr_t d;
        ip_addr_copy_from_ip4(d, dns);
        dns_setserver(0, &d);
        printf("Static IP %s (hostname %s)\n", config.ipaddr, hostname);
    } else {
        printf("DHCP (hostname %s)\n", hostname);
    }
    cyw43_arch_lwip_end();
}

static void connection(int mode)
{
    switch (mode) {
    case CONNECT_WIFI:
        printf("Connecting to WiFi...\n");

        sysstatus = STAT_WIFI_CONNECTING;

        if (strlen(config.wifi_ssid) == 0 ||
            cyw43_arch_wifi_connect_timeout_ms(config.wifi_ssid, config.wifi_passwd,
                                               CYW43_AUTH_WPA2_AES_PSK, 30000)) {
            sysstatus = STAT_WIFI_DISCONNECTED;
            printf("Failed to connect.\n");
            msclog_mark("wifi failed");
            break;
        }

        sysstatus = STAT_WIFI_CONNECTED;

        /* 省電力 (既定) だと応答が 50～300ms 遅れ、netpipe の TCP が遅くなる */
        cyw43_arch_lwip_begin();
        cyw43_wifi_pm(&cyw43_state, cyw43_pm_value(CYW43_NO_POWERSAVE_MODE, 20, 1, 1, 1));
        cyw43_arch_lwip_end();

        ip4_addr_t *address = &(cyw43_state.netif[0].ip_addr);
        printf("Connected to %s as %d.%d.%d.%d as host %s\n",
               config.wifi_ssid,
               ip4_addr1_16(address), ip4_addr2_16(address), ip4_addr3_16(address), ip4_addr4_16(address),
               cyw43_state.netif[0].hostname);

        /* fall through */

    case CONNECT_SMB2:
        if (strlen(config.smb2_server) == 0) {
            printf("Failed to connect SMB2 server\n");
            break;
        }

        sysstatus = STAT_SMB2_CONNECTING;

        struct smb2_context *smb2ipc;

        if ((smb2ipc = connect_smb2("IPC$")) == NULL) {
            msclog_mark("smb2 failed");
            sysstatus = STAT_WIFI_CONNECTED;
            break;
        }

        sysstatus = STAT_SMB2_CONNECTED;
        msclog_mark("smb2 connected");

        boottime = (smb2_get_system_time(smb2ipc) / 10) - (11644473600 * 1000000) - to_us_since_boot(get_absolute_time());
        time_t tt = (time_t)((boottime + to_us_since_boot(get_absolute_time())) / 1000000);
        struct tm *tm = localtime(&tt);
        printf("Boottime UTC %04d/%02d/%02d %02d:%02d:%02d\n", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
#if SD_CARD
        {
            /* SD カード (FatFs) のファイルの時刻 (get_fattime) と time() は always-on タイマー (sdhds.c の sdc_init で 0 から動かしてある) */
            static bool set;
            struct timespec ts = { .tv_sec = tt, .tv_nsec = 0 };
            if (!set) {
                aon_timer_set_time(&ts);
                set = true;
            }
        }
#endif

        disconnect_smb2(smb2ipc);

        /* fall through */

    default:
        break;
    }
}

#if BOOTWAIT_SCREEN
struct bw_ctx {
    struct smb2_context *smb2;
    struct smb2fh *sfh;
};

static int bw_hds_read(void *ctx, uint32_t lba, uint8_t *buf)
{
    struct bw_ctx *c = ctx;
    return hds_cache_read(c->smb2, c->sfh, lba, buf) < 0 ? -1 : 0;
}
#endif

static int vd_mount(void)
{
    uint32_t nvalue;
    struct dir_entry *dirent;
    int len;

    int remoteunit = atoi(config.remoteunit);
    int remoteboot = atoi(config.remoteboot);

    /* Set up remote drive */
    for (int i = 0; i < remoteunit; i++) {
        struct smb2_context *smb2;
        const char *shpath;
        if (sdhds_is(config.remote[i]))
            continue;                   /* SD カードのユニットは vd_init で用意してある */
        if ((smb2 = connect_smb2_path(config.remote[i], &shpath)) == NULL)
            continue;

        struct smb2_stat_64 st;
        if (smb2_stat(smb2, shpath, &st) < 0 || st.smb2_type != SMB2_TYPE_DIRECTORY) {
            printf("%s is not directory.\n", config.remote[i]);
            continue;
        }
        rootpath[i] = config.remote[i];
        printf("REMOTE%u: %s\n", i, config.remote[i]);
    }

    int id = remoteboot ? 1 : 0;

    /* Set up remote HDS */
    for (int i = 0; i < countof(config.hds); i++, id++) {
        struct smb2_context *smb2;
        const char *shpath;
        if (sdhds_is(config.hds[i]))
            continue;                   /* SD カードの上の HDS は vd_init で開いてある */
        if ((smb2 = connect_smb2_path(config.hds[i], &shpath)) == NULL)
            continue;

        struct smb2_stat_64 st;
        if (smb2_stat(smb2, shpath, &st) < 0 || st.smb2_type != SMB2_TYPE_FILE) {
            printf("File %s not found.\n", config.hds[i]);
            msclog_mark("HDS%u not found", i);
            continue;
        }
#if BOOTWAIT_SCREEN
        /* 起動ディスク (ID 0) なら先頭を控えと比べる。USB タスクに見せる (sfh を入れる) 前に済ませる:
           それまで Z からの読み込みは控えか NOT READY で答えている (libsmb2 の同じ接続を 2 つのタスクで使わない) */
        struct smb2fh *fh;
        if ((fh = smb2_open(smb2, shpath, O_RDWR)) == NULL) {
            printf("File %s open failure.\n", config.hds[i]);
            msclog_mark("HDS%u open failure", i);
            continue;
        }
        if (id == 0) {
            struct bw_ctx c = { smb2, fh };
            bw_check(config.hds[i], st.smb2_size, bw_hds_read, &c);
            msclog_mark("snapshot %s", bw_state(config.hds[i]));
        }
        diskinfo[id].smb2 = smb2;
        diskinfo[id].size = st.smb2_size;
        diskinfo[id].sfh = fh;
#else
        if ((diskinfo[id].sfh = smb2_open(smb2, shpath, O_RDWR)) == NULL) {
            printf("File %s open failure.\n", config.hds[i]);
            msclog_mark("HDS%u open failure", i);
            continue;
        }
        diskinfo[id].smb2 = smb2;
        diskinfo[id].size = st.smb2_size;
#endif
        printf("HDS%u: %s size=%lld\n", i, config.hds[i], st.smb2_size);
        msclog_mark("HDS%u opened id%d size=%lld", i, id, st.smb2_size);
    }

    for (int i = 0; i < 7; i++) {
        diskinfo[i].sects = (diskinfo[i].size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    }

    sysstatus = STAT_CONFIGURED;
}

/* 設定画面の SETCONFIG で Wi-Fi か SMB をつなぎ直す前に、SMB の HDS とリモートドライブを外す。
   外さないと古い接続が「開いている」まま残り、次の keepalive までリモートドライブが -14 を返していた
   (Charlie が PZL で見つけた)。SD カードの HDS とユニットはそのまま。つなぎ直したら vd_mount で開き直す */
static void vd_unmount_smb(void)
{
    /* 先に状態を下げる (下の disconnect_smb2_all は SMB が止まっていると数秒かかる。その間に sfh だけが NULL で
       sysstatus が STAT_CONFIGURED のままだと、USB タスクが MEDIUM ERROR を返していた) */
    if (sysstatus > STAT_WIFI_CONNECTED)
        sysstatus = STAT_WIFI_CONNECTED;
    for (int i = 0; i < 8; i++)
        if (rootpath[i] != NULL && !sdhds_is(rootpath[i]))
            rootpath[i] = NULL;
    for (int i = 0; i < 7; i++)
        if (diskinfo[i].type == DTYPE_HDS && diskinfo[i].smb2 != NULL) {
            diskinfo[i].sfh = NULL;     /* USB タスクはこれを見て NOT READY で待たせる */
            diskinfo[i].smb2 = NULL;
        }
    hds_cache_init();
    disconnect_smb2_all();
    if (sysstatus > STAT_WIFI_CONNECTED)
        sysstatus = STAT_WIFI_CONNECTED;
}

/* SD カードの HDS1 を Wi-Fi 越しに用意する: HDS0 と同じ SMB の共有の SDHDS_FETCH1 を、カードに無ければ写す。
   USB タスクと libsmb2 の接続を分けるため、写すための接続を別に作って終わったら閉じる */
static void sd_fetch_once(void)
{
    char share[64];
    const char *slash = strchr(config.hds[0], '/');
    struct smb2_context *smb2;
    if (sysstatus < STAT_SMB2_CONNECTED || !slash || slash == config.hds[0] || sdhds_is(config.hds[0]) ||
        (size_t)(slash - config.hds[0]) >= sizeof share)
        return;
    memcpy(share, config.hds[0], slash - config.hds[0]);
    share[slash - config.hds[0]] = 0;
    if ((smb2 = connect_smb2(share)) == NULL)
        return;
    sdhds_fetch(smb2, SDHDS_FETCH1, SDHDS_AUTO1);
    disconnect_smb2(smb2);
}

//****************************************************************************
// WiFi connection task
//****************************************************************************

void connect_task(void *params)
{
    /* Set up WiFi connection */

    xSemaphoreTake(remote_sem, portMAX_DELAY);
    /* 電源を入れた直後は Wi-Fi や SMB2 サーバへの接続が一度で通らないことがある。通らないまま
       USB 側に知らせると、Z は HDS の無いディスクを読んで「ディスクから起動できません」になり、
       Z の電源を入れ直すまで直らなかった。何度か繰り直す (Z の USB の読み込みはその間待たせる) */
    for (int i = 0; i < 5 && sysstatus < STAT_SMB2_CONNECTED; i++) {
        if (i > 0) {
            printf("Retry connection (%d)\n", i);
            msclog_mark("retry connection %d", i);
            NEXT_IN(1000);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        NEXT_SET(0);
        connection(sysstatus >= STAT_WIFI_CONNECTED ? CONNECT_SMB2 : CONNECT_WIFI);
    }
    if (sysstatus >= STAT_SMB2_CONNECTED) {
        vd_mount();
    }
    msclog_mark("connect done, notify USB (sysstatus %d)", sysstatus);
    sd_fetch_once();
#if HID_KEYBOARD
    connect_finished = true;
#endif
    xSemaphoreGive(remote_sem);
    xTaskNotify(main_th, 1, eSetBits);

#if BOOTWAIT
    /* 5 回で通らなくても諦めず、裏で繰り直し続ける (smbd が遅れて上がる、Wi-Fi が遠い など)。
       その間 Z からの HDS の読み書きは NOT READY で待たせている (virtual_disk.c / msc_disk.c) */
    for (int i = 5; sysstatus < STAT_CONFIGURED && strlen(config.wifi_ssid) > 0 && strlen(config.smb2_server) > 0; i++) {
        NEXT_IN(5000);
        vTaskDelay(pdMS_TO_TICKS(5000));
        xSemaphoreTake(remote_sem, portMAX_DELAY);
        NEXT_SET(0);
        if (sysstatus < STAT_SMB2_CONNECTED) {
            printf("Retry connection (%d)\n", i);
            msclog_mark("retry connection %d", i);
            connection(sysstatus >= STAT_WIFI_CONNECTED ? CONNECT_SMB2 : CONNECT_WIFI);
        }
        if (sysstatus >= STAT_SMB2_CONNECTED) {
            vd_mount();
            msclog_mark("connected after retry %d (sysstatus %d)", i, sysstatus);
        }
        xSemaphoreGive(remote_sem);
    }
#endif
    NEXT_SET(UINT32_MAX);

    while (1) {
        uint32_t nvalue = 0;
        int mode;

        if (smb_down) {
            /* smb_lost のあと: 通るまで 5 秒ごとにつなぎ直す (Wi-Fi が切れていれば Wi-Fi から) */
            if (xTaskNotifyWait(1, 0, &nvalue, pdMS_TO_TICKS(5000)) != pdTRUE || !(nvalue & CONNECT_WAIT))
                nvalue = CONNECT_WAIT | (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP ?
                                         CONNECT_SMB2 : CONNECT_WIFI);
        } else {
            xTaskNotifyWait(1, 0, &nvalue, portMAX_DELAY);
            if (!(nvalue & CONNECT_WAIT))
                continue;
        }
        xSemaphoreTake(remote_sem, portMAX_DELAY);
        mode = nvalue & CONNECT_MASK;
        if (mode == CONNECT_WIFI || mode == CONNECT_SMB2)
            vd_unmount_smb();
        connection(mode);
        if ((mode == CONNECT_WIFI || mode == CONNECT_SMB2) && sysstatus >= STAT_SMB2_CONNECTED)
            vd_mount();
        if (smb_down && sysstatus >= STAT_CONFIGURED) {
            smb_down = false;
            printf("SMB reconnected\n");
            msclog_mark("smb reconnected");
        }
        xSemaphoreGive(remote_sem);
    }
}

//****************************************************************************
// Remote connection keepalive task
//****************************************************************************

/* SMB の接続 (smb2_context) は HDS の読み書きをする USB タスクと共有している。libsmb2 は同じ接続を 2 つのタスクから
   同時に使えない (keepalive の smb2_stat と USB タスクの smb2_read が重なると中が壊れ、"Out of memory" で落ちた。
   最初の keepalive は接続した直後に来るので、Z が起動のために HDS を読んでいる最中と重なりやすい)。
   keepalive は印を立てるだけにして、USB タスクが自分の番に keepalive_poll で行う */
static volatile bool keepalive_due;

/* SMB の接続が切れた (または SMB_IO_TIMEOUT 秒答えが無かった) ときに、USB タスクから呼ぶ。libsmb2 は打ち切った要求の
   遅れた答えを受けると接続ごと使えなくなるので、その接続はもう使わない: smb_down を立てて、USB タスクの HDS の読み書きは
   SMB に触らずに NOT READY を返す (virtual_disk.c)。つなぎ直しは接続タスクが vd_unmount_smb → 接続 → vd_mount で行い、
   開けたら smb_down を下ろす */
volatile bool smb_down;

void smb_lost(const char *why)
{
    if (smb_down)
        return;
    smb_down = true;
    printf("SMB lost (%s), reconnecting\n", why);
    msclog_mark("smb lost (%s)", why);
    xTaskNotify(connect_th, CONNECT_SMB2 | CONNECT_WAIT, eSetBits);
}

void keepalive_poll(void)
{
    if (!keepalive_due || sysstatus < STAT_SMB2_CONNECTED || smb_down)
        return;
    if (xSemaphoreTake(remote_sem, 0) != pdTRUE)
        return;                         /* 接続タスクが繋ぎにいっている: 次の周で */
    keepalive_due = false;
    keepalive_smb2_all();
    xSemaphoreGive(remote_sem);
}

void keepalive_task(void *params)
{
    while (1) {
        TickType_t delay;
        if (sysstatus >= STAT_SMB2_CONNECTED) {
            keepalive_due = true;
            delay = pdMS_TO_TICKS(5 * 60 * 1000);
        } else {
            delay = pdMS_TO_TICKS(30 * 1000);
        }
        vTaskDelay(delay);

        extern char __HeapLimit;
        struct mallinfo mi = mallinfo();
        printf("arena=%d used=%d free=%d", mi.arena, mi.uordblks, mi.fordblks);
        printf(" heapfree=%d\n", &__HeapLimit - (char *)sbrk(0));

        time_t tt = (time_t)((boottime + to_us_since_boot(get_absolute_time())) / 1000000);
        struct tm *tm = localtime(&tt);
        printf("%04d/%02d/%02d %02d:%02d:%02d\n", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

        static TaskStatus_t pxTaskStatusArray[16];   /* 8 では足りず (タスクが 9 つ以上) 何も出ていなかった */
        unsigned long ulTotalRunTime;
        int uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, count_of(pxTaskStatusArray), &ulTotalRunTime);
        printf("ID Task Name        S Pr Stack\n");
        for (int x = 0; x < uxArraySize; x++) {
            printf("%2u %-16s %c %2u %5u\n",
                   pxTaskStatusArray[x].xTaskNumber,
                   pxTaskStatusArray[x].pcTaskName,
                   "RRBSD"[pxTaskStatusArray[x].eCurrentState],
                   pxTaskStatusArray[x].uxCurrentPriority,
                   pxTaskStatusArray[x].usStackHighWaterMark);
        }
    }
}
