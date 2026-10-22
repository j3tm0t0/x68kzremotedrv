/*
 * hidkbd.c -- USB キーボード (HID) を MSC と同じデバイスに足し、Wi-Fi (UDP 6869) で受けたコマンドどおりにキーを押す
 *
 * 別の Pico W で動かしていた picokbd (x68k の harness/picokbd) をこのファームに取り込んだもの。
 * Raspberry Pi などの x68key (PICOKBD=この Pico の IP) から ESC で rcmd を止めたり、Ctrl+F11+Del でリセットしたりする。
 * コマンドは hidkbd_cmd.c。終わったら "ok" (解釈できなければ "err ...") を返す。実行中に来たものには "err busy"。
 *
 * picokbd は押すたびに tud_task() を回しながら待っていたが、ここの USB タスクは Z の MSC (SCSI) も受け持つので
 * 待たない: lwIP スレッドの受信コールバックは 1 行を写すだけ、USB タスクは毎周 hidkbd_task() で
 * 「時刻が来ていて、HID の送信口が空いていれば次のレポートを送る」だけをする。
 * 6869 の口は USB を始める前に開く (Z が netpipe を使っていなくても、Z が固まっていてもリセットできるように。
 * USB タスクが vd_sync で SMB の接続を待っている間も status は答える)。
 *
 * "status" だけはキーを押さず、専用の小さなタスクが 1 行で答える (キーの実行中でも、Z が USB を読みに来なくても):
 *   ok stat=<sysstatus> wifi=up|connecting|down|failed ip=a.b.c.d rssi=-55 smb=mounted|connected|connecting|failed|waiting
 *      hds0=open|closed ... remote0=ok|none ... uptime=812s usb=mounted|unmounted reads=<セクタ> writes=<セクタ>
 *      npd=<netpipe の交換回数> accel=ready|busy|wait|off/<計算した数> hid=ok|busy|notready heap=<空き>
 * 状態は x68kzremotedrv の sysstatus (STAT_*) と diskinfo / rootpath をそのまま読む。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/sync.h"
#include "lwip/udp.h"
#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"

#include "main.h"
#include "vd_command.h"
#include "config_file.h"
#include "virtual_disk.h"
#include "netpipe.h"
#include "accel.h"
#include "bootwait.h"
#include "hidkbd_cmd.h"
#include "hidkbd.h"

#define HIDKBD_PORT     6869
#define READY_TIMEOUT   1000            /* Z が HID を読みに来ないまま (ms) なら諦める */

static struct udp_pcb *kpcb;
static char line[512];
static volatile bool pending;           /* line にコマンドがある (lwIP スレッドが立て、USB タスクが終わったら下ろす) */
static ip_addr_t from;
static u16_t from_port;

/* status の要求 (lwIP スレッドが立て、status タスクが答える) */
static volatile bool st_pending;
static ip_addr_t st_from;
static u16_t st_port;
static TaskHandle_t st_th;

extern volatile bool connect_finished;          /* connect.c */
extern const char *rootpath[8];                 /* connect.c */
extern volatile uint32_t msc_reads, msc_writes; /* msc_disk.c */
#if BOOTWAIT
extern volatile uint32_t msc_notready, msc_ioerr;
#endif

/* USB タスクだけが触る */
static struct hidkbd_cmd cmd;
static struct hidkbd_report rep;
static bool running, have_rep;
static uint32_t due, wait_from;

/* lwIP スレッドの中で呼ぶ */
static void sendto_locked(const char *s, const ip_addr_t *addr, u16_t port)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, strlen(s), PBUF_RAM);
    if (p == NULL)
        return;
    memcpy(p->payload, s, strlen(s));
    udp_sendto(kpcb, p, addr, port);
    pbuf_free(p);
}


static void on_udp(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    char w[8];
    int n = pbuf_copy_partial(p, w, sizeof w - 1, 0);
    while (n > 0 && (w[n - 1] == '\n' || w[n - 1] == '\r' || w[n - 1] == ' '))
        n--;
    w[n] = '\0';
    if (strcmp(w, "status") == 0) {
        st_from = *addr;
        st_port = port;
        __dmb();
        st_pending = true;
        if (st_th != NULL)
            xTaskNotifyGive(st_th);
    } else if (pending) {
        sendto_locked("err busy", addr, port);
    } else {
        int n = pbuf_copy_partial(p, line, sizeof line - 1, 0);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            n--;
        line[n] = '\0';
        from = *addr;
        from_port = port;
        __dmb();
        pending = true;
    }
    pbuf_free(p);
}

/* 返事をして次のコマンドを受け付ける。返事より先に pending を下ろす (返事を受けてすぐ次を送る相手に "err busy" を返さない) */
static void finish(const char *s)
{
    ip_addr_t to = from;
    u16_t port = from_port;
    running = have_rep = false;
    __dmb();
    pending = false;
    cyw43_arch_lwip_begin();
    sendto_locked(s, &to, port);
    cyw43_arch_lwip_end();
}

static const char *wifi_state(void)
{
    if (sysstatus >= STAT_WIFI_CONNECTED)
        return "up";
    if (sysstatus == STAT_WIFI_CONNECTING)
        return "connecting";
    return connect_finished ? "failed" : "down";
}

static const char *smb_state(void)
{
    switch (sysstatus) {
    case STAT_CONFIGURED:       return "mounted";
    case STAT_SMB2_CONNECTED:   return "connected";
    case STAT_SMB2_CONNECTING:  return "connecting";
    }
    return connect_finished ? (BOOTWAIT ? "retrying" : "failed") : "waiting";
}

/* status の 1 行を作る。cyw43_arch_lwip_begin の中で呼ぶ (RSSI を cyw43 に聞く) */
static int status_line(char *s, int size)
{
    int n = snprintf(s, size, "ok stat=%d wifi=%s", sysstatus, wifi_state());
    if (sysstatus >= STAT_WIFI_CONNECTED) {
        char ip[16];
        int32_t rssi = 0;
        ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), ip, sizeof ip);
        n += snprintf(s + n, size - n, " ip=%s", ip);
        if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0)
            n += snprintf(s + n, size - n, " rssi=%ld", (long)rssi);
    }
    n += snprintf(s + n, size - n, " smb=%s", smb_state());
    int id = atoi(config.remoteboot) ? 1 : 0;   /* connect.c の vd_mount と同じ数え方 */
    for (int i = 0; i < countof(config.hds); i++, id++)
        if (config.hds[i][0] != '\0' && n < size)
            n += snprintf(s + n, size - n, " hds%d=%s", i, diskinfo[id].sfh != NULL ? "open" : "closed");
    for (int i = 0; i < atoi(config.remoteunit) && i < 8 && n < size; i++)
        n += snprintf(s + n, size - n, " remote%d=%s", i, rootpath[i] != NULL ? "ok" : "none");
    uint32_t done;
    const char *acc = accel_state(&done);
    extern char __HeapLimit;
    if (n < size)
        n += snprintf(s + n, size - n, " uptime=%lus usb=%s reads=%lu writes=%lu npd=%lu accel=%s/%lu hid=%s heap=%d",
                      (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000), tud_mounted() ? "mounted" : "unmounted",
                      (unsigned long)msc_reads, (unsigned long)msc_writes, (unsigned long)netpipe_xchg, acc, (unsigned long)done,
                      pending ? "busy" : (tud_hid_ready() ? "ok" : "notready"), (int)(&__HeapLimit - (char *)sbrk(0)));
#if BOOTWAIT
    if (n < size)
        n += snprintf(s + n, size - n, " notready=%lu ioerr=%lu", (unsigned long)msc_notready, (unsigned long)msc_ioerr);
#endif
#if BOOTWAIT_SCREEN
    if (n < size)
        n += snprintf(s + n, size - n, " snap=%s", bw_state(config.hds[0]));
#endif
    return n < size ? n : size - 1;
}

/* status に答える。USB タスクとは別 (USB タスクは vd_sync で SMB の接続を待つことがある) */
static void status_task(void *params)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!st_pending)
            continue;
        ip_addr_t to = st_from;
        u16_t port = st_port;
        __dmb();
        st_pending = false;
        char s[400];
        cyw43_arch_lwip_begin();
        status_line(s, sizeof s);
        sendto_locked(s, &to, port);
        cyw43_arch_lwip_end();
    }
}

void hidkbd_init(void)
{
    xTaskCreate(status_task, "HidKbdStatus", 768, NULL, 1, &st_th);
    cyw43_arch_lwip_begin();
    kpcb = udp_new();
    if (kpcb != NULL) {
        udp_bind(kpcb, IP_ANY_TYPE, HIDKBD_PORT);
        udp_recv(kpcb, on_udp, NULL);
    }
    cyw43_arch_lwip_end();
    printf("hidkbd: %s (udp %d)\n", kpcb != NULL ? "ready" : "no udp pcb", HIDKBD_PORT);
}

void hidkbd_task(void)
{
    if (kpcb == NULL || !pending)
        return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!running) {
        __dmb();
        const char *r = hidkbd_parse(&cmd, line);
        if (r != NULL) {
            finish(r);
            return;
        }
        running = true;
        due = now;
    }
    if ((int32_t)(now - due) < 0)
        return;
    if (!have_rep) {
        if (!hidkbd_next(&cmd, &rep)) {
            finish("ok");
            return;
        }
        have_rep = true;
        wait_from = now;
    }
    if (!tud_hid_ready()) {
        if (now - wait_from > READY_TIMEOUT)
            finish("err usb");
        return;
    }
    tud_hid_keyboard_report(0, rep.mod, rep.keys);
    have_rep = false;
    due = now + rep.ms;
}

/* HID の必須コールバック (使わない。LED の状態などは捨てる) */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
}
