/*
 * netpipe.c -- SCSI ID 6 の空き LBA 帯を使って、X68000 Z と Wi-Fi 側のホストの間でバイト列 (SLIP) を運ぶ
 *
 * X68000 Z (slipd.x -z) は 1 回の交換で
 *   1. NETPIPE_WLBA に要求 (見出し 16 バイト + 送信データ) を書き、
 *   2. NETPIPE_RLBA 帯のどこか (読むたびに 8 ずつ下げる) から応答 (見出し 16 バイト + 受信データ) を読む。
 * 送信データはそのまま UDP で SMB2 サーバ (config.smb2_server) の NETPIPE_PORT へ送り、
 * そこから届いた UDP の中身は受信リングに溜めて、次の応答で返す。中身の解釈 (SLIP) は両端がする。
 */

#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/sync.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"

#include "main.h"
#include "config_file.h"
#include "vd_command.h"
#include "netpipe.h"

#define NETPIPE_PORT    6868
#define NETPIPE_MAGIC   0x58364e50      /* "X6NP" (big endian) */
#define NETPIPE_SECTS   8
#define NETPIPE_DATAMAX (NETPIPE_SECTS * 512 - 16)
#define UDP_CHUNK       1400            /* IP の断片化を避ける */
#define RXRING          16384

struct np_header {
    uint32_t magic;
    uint32_t seq;                       /* 要求の通し番号 (応答はこれを返す) */
    uint16_t len;                       /* 要求: 送信データ長 / 応答: 受信データ長 */
    uint16_t rxmax;                     /* 要求: 受け取れる最大長 / 応答: リングに残っている長さ */
    uint32_t stat;                      /* 応答: 捨てた UDP の数 */
};

static uint8_t reqbuf[NETPIPE_SECTS * 512];
static uint8_t resbuf[NETPIPE_SECTS * 512];
static uint8_t rxring[RXRING];
static volatile uint32_t rx_head, rx_tail;  /* head: lwIP スレッドが書く / tail: USB タスクが読む */
static volatile uint32_t rx_drops;
static struct udp_pcb *pcb;
static ip_addr_t peer;
static uint32_t last_tx;

extern volatile int sysstatus;

/* 16/32 ビットの値は X68000 側に合わせてビッグエンディアン */
static uint16_t be16(uint16_t v) { return (v >> 8) | (v << 8); }
static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

static void np_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    uint32_t used = rx_head - rx_tail;
    if (p->tot_len > RXRING - used) {
        rx_drops++;
    } else {
        uint32_t h = rx_head;
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            const uint8_t *s = q->payload;
            for (int i = 0; i < q->len; i++)
                rxring[h++ % RXRING] = s[i];
        }
        __dmb();
        rx_head = h;
    }
    pbuf_free(p);
}

static bool np_open(void)
{
    if (pcb != NULL)
        return true;
    if (sysstatus < STAT_WIFI_CONNECTED || !ipaddr_aton(config.smb2_server, &peer))
        return false;
    cyw43_arch_lwip_begin();
    pcb = udp_new();
    if (pcb != NULL) {
        udp_bind(pcb, IP_ANY_TYPE, NETPIPE_PORT);
        udp_recv(pcb, np_recv, NULL);
    }
    cyw43_arch_lwip_end();
    if (pcb != NULL)
        printf("netpipe: udp %s:%d\n", config.smb2_server, NETPIPE_PORT);
    return pcb != NULL;
}

static void np_send(const uint8_t *data, int len)
{
    while (len > 0) {
        int n = len > UDP_CHUNK ? UDP_CHUNK : len;
        cyw43_arch_lwip_begin();
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
        if (p != NULL) {
            pbuf_take(p, data, n);
            udp_sendto(pcb, p, &peer, NETPIPE_PORT);
            pbuf_free(p);
        }
        cyw43_arch_lwip_end();
        data += n;
        len -= n;
    }
    last_tx = to_ms_since_boot(get_absolute_time());
}

/* 要求がそろったら呼ぶ: 送信して、応答を resbuf に作る */
static void np_exchange(void)
{
    struct np_header *rq = (struct np_header *)reqbuf;
    struct np_header *rs = (struct np_header *)resbuf;
    int txlen = be16(rq->len);
    int rxmax = be16(rq->rxmax);
    int n = 0;

    if (np_open()) {
        if (txlen > 0) {
            np_send(reqbuf + 16, txlen);
        } else if (to_ms_since_boot(get_absolute_time()) - last_tx > 2000) {
            /* 相手 (Pi) にこちらの宛先を知らせる。SLIP の END 1 バイトは空パケットとして捨てられる */
            static const uint8_t end = 0xc0;
            np_send(&end, 1);
        }
        if (rxmax > NETPIPE_DATAMAX)
            rxmax = NETPIPE_DATAMAX;
        uint32_t t = rx_tail;
        uint32_t avail = rx_head - t;
        __dmb();
        n = avail < rxmax ? avail : rxmax;
        for (int i = 0; i < n; i++)
            resbuf[16 + i] = rxring[t++ % RXRING];
        rx_tail = t;
    }
    rs->magic = rq->magic;
    rs->seq = rq->seq;
    rs->len = be16(n);
    rs->rxmax = be16((uint16_t)(rx_head - rx_tail));
    rs->stat = be32(rx_drops);
}

int netpipe_write(uint32_t lba, const uint8_t *buf)
{
    static int nsect;
    int page = lba - NETPIPE_WLBA;
    if (page < 0 || page >= NETPIPE_SECTS)
        return -1;
    if (page == 0) {
        const struct np_header *rq = (const struct np_header *)buf;
        if (rq->magic != be32(NETPIPE_MAGIC) || be16(rq->len) > NETPIPE_DATAMAX)
            return -1;
        nsect = (16 + be16(rq->len) + 511) / 512;
    }
    memcpy(&reqbuf[page * 512], buf, 512);
    if (page == nsect - 1)
        np_exchange();
    return 0;
}

int netpipe_read(uint32_t lba, uint8_t *buf)
{
    memcpy(buf, &resbuf[(lba % NETPIPE_SECTS) * 512], 512);
    return 0;
}
