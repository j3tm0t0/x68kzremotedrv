/*
 * netpipe.c -- SCSI ID 6 の空き LBA 帯を使って、X68000 Z (slipd.x -z) と Wi-Fi の間で SLIP を運ぶ
 *
 * X68000 Z は 1 回の交換で
 *   1. NETPIPE_WLBA に要求 (見出し 16 バイト + 送信データ) を書き、
 *   2. NETPIPE_RLBA 帯のどこか (読むたびに 8 ずつ下げる) から応答 (見出し 16 バイト + 受信データ) を読む。
 * 送信データも受信データも SLIP (RFC 1055) のバイト列。運び方は 2 通りあり、要求の見出しで Z が選ぶ。
 *
 *   ブリッジ (要求の +12 に Z の IP アドレスがあるとき):
 *     Pico が Z の IP アドレスを代わりに名乗る (Z 宛ての ARP 要求に Pico の MAC で答える)。
 *     Z 宛ての IPv4 パケットは Wi-Fi から横取りして SLIP にして Z に渡し、Z からの SLIP はほどいて
 *     lwIP の etharp_output で Wi-Fi に出す (行き先が LAN の外ならゲートウェイへ)。Z は LAN 上の 1 台になる。
 *   UDP (+12 が 0 のとき):
 *     SLIP のバイト列をそのまま UDP で SMB2 サーバ (config.smb2_server) の NETPIPE_PORT へ送り、
 *     届いた UDP の中身を Z に返す。相手 (Raspberry Pi の slipudp.py + slattach) が SLIP をほどく。
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
#include "lwip/etharp.h"
#include "lwip/prot/ethernet.h"
#include "lwip/prot/etharp.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/iana.h"

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
#define MTU             1500

#define SLIP_END        0xc0
#define SLIP_ESC        0xdb
#define SLIP_ESC_END    0xdc
#define SLIP_ESC_ESC    0xdd

struct np_header {
    uint32_t magic;
    uint32_t seq;                       /* 要求の通し番号 (応答はこれを返す) */
    uint16_t len;                       /* 要求: 送信データ長 / 応答: 受信データ長 */
    uint16_t rxmax;                     /* 要求: 受け取れる最大長 / 応答: リングに残っている長さ */
    uint32_t stat;                      /* 要求: Z の IP アドレス (0 なら UDP) / 応答: 統計 (下記) */
};

static uint8_t reqbuf[NETPIPE_SECTS * 512];
static uint8_t resbuf[NETPIPE_SECTS * 512];

/* Z へ渡す SLIP のバイト列。head: 受信側 (lwIP スレッドまたは cyw43 の処理) が書く / tail: USB タスクが読む */
static uint8_t rxring[RXRING];
static volatile uint32_t rx_head, rx_tail;

/* 統計 (応答の stat: 上位 16 ビット = Z に渡したパケット数, 下位 16 ビット = 捨てた数) */
static volatile uint32_t st_tozf, st_drop;

static struct udp_pcb *pcb;
static ip_addr_t peer;
static uint32_t last_tx;

/* ブリッジ */
static volatile uint32_t zip;           /* Z の IP アドレス (ネットワークバイトオーダー)。0 ならブリッジしない */
static struct netif *wifi;
static netif_input_fn wifi_input;       /* 横取りする前の netif->input */
static volatile bool arp_pending;       /* Z 宛ての ARP 要求に答える (USB タスクで送る) */
static struct eth_addr arp_mac;
static ip4_addr_t arp_ip;
static bool announced;                  /* Z の IP アドレスを名乗った (gratuitous ARP) */

extern volatile int sysstatus;

/* 16/32 ビットの値は X68000 側に合わせてビッグエンディアン */
static uint16_t be16(uint16_t v) { return (v >> 8) | (v << 8); }
static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

/* リングの空き。足りなければ false */
static bool ring_room(uint32_t n)
{
    return n <= RXRING - (rx_head - rx_tail);
}

/* SLIP にしてリングに積む (入りきらないときは丸ごと捨てる) */
static void ring_put_slip(const struct pbuf *p, uint16_t offset)
{
    if (!ring_room(2 * (p->tot_len - offset) + 2)) {
        st_drop++;
        return;
    }
    uint32_t h = rx_head;
    rxring[h++ % RXRING] = SLIP_END;
    uint16_t skip = offset;
    for (const struct pbuf *q = p; q != NULL; q = q->next) {
        const uint8_t *s = q->payload;
        uint16_t i = 0;
        if (skip >= q->len) {
            skip -= q->len;
            continue;
        }
        i = skip;
        skip = 0;
        for (; i < q->len; i++) {
            uint8_t c = s[i];
            if (c == SLIP_END) {
                rxring[h++ % RXRING] = SLIP_ESC;
                rxring[h++ % RXRING] = SLIP_ESC_END;
            } else if (c == SLIP_ESC) {
                rxring[h++ % RXRING] = SLIP_ESC;
                rxring[h++ % RXRING] = SLIP_ESC_ESC;
            } else {
                rxring[h++ % RXRING] = c;
            }
        }
    }
    rxring[h++ % RXRING] = SLIP_END;
    __dmb();
    rx_head = h;
    st_tozf++;
}

/****************************************************************************
 * UDP
 ****************************************************************************/

static void np_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    if (zip == 0) {                     /* ブリッジ中は Pi からの UDP を混ぜない */
        if (!ring_room(p->tot_len)) {
            st_drop++;
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
    }
    pbuf_free(p);
}

static void np_send(const uint8_t *data, int len)
{
    while (len > 0) {
        int n = len > UDP_CHUNK ? UDP_CHUNK : len;
        cyw43_arch_lwip_begin();
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_POOL);
        if (p != NULL) {
            pbuf_take(p, data, n);
            udp_sendto(pcb, p, &peer, NETPIPE_PORT);
            pbuf_free(p);
        } else {
            st_drop++;
        }
        cyw43_arch_lwip_end();
        data += n;
        len -= n;
    }
    last_tx = to_ms_since_boot(get_absolute_time());
}

/****************************************************************************
 * ブリッジ
 ****************************************************************************/

/* Wi-Fi から届いたフレーム (cyw43 の受信処理から呼ばれる)。Z 宛てのものだけ取り、ほかは lwIP に渡す。
 * ここでは lwIP の関数を呼ばない (ロックを取らない)。返答の ARP は USB タスクで送る */
static err_t np_input(struct pbuf *p, struct netif *netif)
{
    uint32_t z = zip;
    if (z != 0 && p->len >= SIZEOF_ETH_HDR + 20) {
        const struct eth_hdr *eh = p->payload;
        if (eh->type == PP_HTONS(ETHTYPE_ARP) && p->len >= SIZEOF_ETH_HDR + SIZEOF_ETHARP_HDR) {
            const struct etharp_hdr *ah = (const struct etharp_hdr *)((const uint8_t *)p->payload + SIZEOF_ETH_HDR);
            uint32_t tip;
            memcpy(&tip, &ah->dipaddr, 4);
            if (ah->opcode == PP_HTONS(ARP_REQUEST) && tip == z) {
                if (!arp_pending) {
                    arp_mac = ah->shwaddr;
                    memcpy(&arp_ip, &ah->sipaddr, 4);
                    __dmb();
                    arp_pending = true;
                }
                pbuf_free(p);
                return ERR_OK;
            }
        } else if (eh->type == PP_HTONS(ETHTYPE_IP)) {
            const struct ip_hdr *ih = (const struct ip_hdr *)((const uint8_t *)p->payload + SIZEOF_ETH_HDR);
            uint32_t dst;
            memcpy(&dst, &ih->dest, 4);
            if (dst == z) {
                const uint8_t *ip = (const uint8_t *)ih;
                int hl = (ip[0] & 15) * 4;
                /* 外からの ping は捨てる: ESP/X は TCP を閉じたあとに echo request を受けると止まった */
                if (IPH_PROTO(ih) == IP_PROTO_ICMP && p->len >= SIZEOF_ETH_HDR + hl + 1 && ip[hl] == 8)
                    st_drop++;
                else
                    ring_put_slip(p, SIZEOF_ETH_HDR);
                pbuf_free(p);
                return ERR_OK;
            }
        }
    }
    return wifi_input(p, netif);
}

/* ARP の返答 (または名乗り) を送る。cyw43_arch_lwip_begin の中で呼ぶ */
static void send_arp(uint16_t op, const struct eth_addr *dmac, const ip4_addr_t *dip)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, SIZEOF_ETH_HDR + SIZEOF_ETHARP_HDR, PBUF_RAM);
    if (p == NULL)
        return;
    struct eth_hdr *eh = p->payload;
    struct etharp_hdr *ah = (struct etharp_hdr *)((uint8_t *)p->payload + SIZEOF_ETH_HDR);
    uint32_t z = zip;
    memcpy(&eh->dest, dmac, ETH_HWADDR_LEN);
    memcpy(&eh->src, wifi->hwaddr, ETH_HWADDR_LEN);
    eh->type = PP_HTONS(ETHTYPE_ARP);
    ah->hwtype = PP_HTONS(LWIP_IANA_HWTYPE_ETHERNET);
    ah->proto = PP_HTONS(ETHTYPE_IP);
    ah->hwlen = ETH_HWADDR_LEN;
    ah->protolen = sizeof(ip4_addr_t);
    ah->opcode = lwip_htons(op);
    memcpy(&ah->shwaddr, wifi->hwaddr, ETH_HWADDR_LEN);
    memcpy(&ah->sipaddr, &z, 4);
    memcpy(&ah->dhwaddr, op == ARP_REPLY ? dmac->addr : (const uint8_t[6]){ 0 }, ETH_HWADDR_LEN);
    memcpy(&ah->dipaddr, op == ARP_REPLY ? (const void *)dip : (const void *)&z, 4);
    wifi->linkoutput(wifi, p);
    pbuf_free(p);
}

/* Z から来た SLIP をほどき、1 パケットそろうたびに Wi-Fi に出す */
static void bridge_out(const uint8_t *s, int n)
{
    static uint8_t pkt[MTU];
    static int pos, esc, bad;
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == SLIP_END) {
            /* IPv4 で、全長 = 受け取った長さ のものだけ出す */
            if (pos >= 20 && !bad && (pkt[0] >> 4) == 4 && ((pkt[2] << 8) | pkt[3]) == pos) {
                struct pbuf *p = pbuf_alloc(PBUF_IP, pos, PBUF_POOL);
                if (p != NULL) {
                    ip4_addr_t dst;
                    pbuf_take(p, pkt, pos);
                    memcpy(&dst, &pkt[16], 4);
                    cyw43_arch_lwip_begin();
                    etharp_output(wifi, p, &dst);   /* LAN の外ならゲートウェイの MAC で出す */
                    cyw43_arch_lwip_end();
                    pbuf_free(p);
                } else {
                    st_drop++;
                }
            }
            pos = esc = bad = 0;
            continue;
        }
        if (c == SLIP_ESC) {
            esc = 1;
            continue;
        }
        if (esc) {
            c = c == SLIP_ESC_END ? SLIP_END : c == SLIP_ESC_ESC ? SLIP_ESC : c;
            esc = 0;
        }
        if (pos < MTU)
            pkt[pos++] = c;
        else
            bad = 1;
    }
}

/****************************************************************************
 * 交換
 ****************************************************************************/

static bool np_open(void)
{
    if (pcb != NULL)
        return true;
    if (sysstatus < STAT_WIFI_CONNECTED)
        return false;
    cyw43_arch_lwip_begin();
    wifi = &cyw43_state.netif[CYW43_ITF_STA];
    if (wifi_input == NULL) {
        wifi_input = wifi->input;
        wifi->input = np_input;
    }
    pcb = udp_new();
    if (pcb != NULL) {
        udp_bind(pcb, IP_ANY_TYPE, NETPIPE_PORT);
        udp_recv(pcb, np_recv, NULL);
    }
    cyw43_arch_lwip_end();
    if (!ipaddr_aton(config.smb2_server, &peer))
        ip_addr_set_zero(&peer);
    if (pcb != NULL)
        printf("netpipe: ready (udp %s:%d)\n", config.smb2_server, NETPIPE_PORT);
    return pcb != NULL;
}

/* 要求がそろったら呼ぶ: 送信して、応答を resbuf に作る */
static void np_exchange(void)
{
    struct np_header *rq = (struct np_header *)reqbuf;
    struct np_header *rs = (struct np_header *)resbuf;
    int txlen = be16(rq->len);
    int rxmax = be16(rq->rxmax);
    uint32_t z = rq->stat;              /* ネットワークバイトオーダーのまま (X68000 もビッグエンディアン) */
    int n = 0;

    if (np_open()) {
        if (z != zip) {
            zip = z;
            announced = false;
            rx_tail = rx_head;          /* 運び方が変わったら残りは捨てる */
            if (z != 0)
                printf("netpipe: bridge %d.%d.%d.%d\n", z & 255, (z >> 8) & 255, (z >> 16) & 255, z >> 24);
        }
        if (z != 0) {
            cyw43_arch_lwip_begin();
            if (!announced) {
                static const struct eth_addr bcast = {{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }};
                send_arp(ARP_REQUEST, &bcast, NULL);    /* gratuitous ARP: 周りの ARP キャッシュを更新させる */
                announced = true;
            }
            if (arp_pending) {
                struct eth_addr m = arp_mac;
                ip4_addr_t ip = arp_ip;
                arp_pending = false;
                send_arp(ARP_REPLY, &m, &ip);
            }
            cyw43_arch_lwip_end();
            if (txlen > 0)
                bridge_out(reqbuf + 16, txlen);
        } else if (txlen > 0) {
            np_send(reqbuf + 16, txlen);
        } else if (to_ms_since_boot(get_absolute_time()) - last_tx > 2000) {
            /* 相手 (Pi) にこちらの宛先を知らせる。SLIP の END 1 バイトは空パケットとして捨てられる */
            static const uint8_t end = SLIP_END;
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
    rs->stat = be32((st_tozf << 16) | (st_drop & 0xffff));
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
