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
 * 6869 の口は Wi-Fi がつながったら開く (Z が netpipe を使っていなくても、Z が固まっていてもリセットできるように)。
 */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/sync.h"
#include "lwip/udp.h"
#include "tusb.h"

#include "main.h"
#include "vd_command.h"
#include "hidkbd_cmd.h"
#include "hidkbd.h"

#define HIDKBD_PORT     6869
#define READY_TIMEOUT   1000            /* Z が HID を読みに来ないまま (ms) なら諦める */

static struct udp_pcb *kpcb;
static char line[512];
static volatile bool pending;           /* line にコマンドがある (lwIP スレッドが立て、USB タスクが終わったら下ろす) */
static ip_addr_t from;
static u16_t from_port;

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
    if (pending) {
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

void hidkbd_task(void)
{
    if (kpcb == NULL) {
        if (sysstatus < STAT_WIFI_CONNECTED)
            return;
        cyw43_arch_lwip_begin();
        kpcb = udp_new();
        if (kpcb != NULL) {
            udp_bind(kpcb, IP_ANY_TYPE, HIDKBD_PORT);
            udp_recv(kpcb, on_udp, NULL);
        }
        cyw43_arch_lwip_end();
        if (kpcb != NULL)
            printf("hidkbd: ready (udp %d)\n", HIDKBD_PORT);
        return;
    }
    if (!pending)
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
