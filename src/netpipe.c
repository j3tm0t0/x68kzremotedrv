/*
 * netpipe.c -- SCSI ID 6 の空き LBA 帯を使って、X68000 Z (slipd.x -z) と Wi-Fi の間で SLIP を運ぶ
 *
 * X68000 Z は 1 回の交換で
 *   1. NETPIPE_WLBA に要求 (見出し 16 バイト + 送信データ) を書き、
 *   2. NETPIPE_RLBA 帯のどこか (読むたびに 8 ずつ下げる) から応答 (見出し 16 バイト + 受信データ) を読む。
 * 送信データも受信データも SLIP (RFC 1055) のバイト列。運び方は 2 通りあり、要求の見出しで Z が選ぶ。
 * ブリッジでは、要求のマジックを "X6NR" にすると SLIP をやめ、IP パケットを長さ付きのレコードで運ぶ
 * (Z の 68000 では SLIP の解読と 1 バイトずつの取り出しが CPU の 2 割を超えていた):
 *   Z へ:   [長さ 2][空き 14][IP パケット][偶数にそろえる詰め物]  (空きは slip.sys が Ethernet の見出しを書く場所)
 *   Z から: [長さ 2][IP パケット][偶数にそろえる詰め物]
 *   中身の先頭が ACCEL_MARK (0xA5) のレコードは IP ではなく暗号アクセラレータの要求 / 応答 (accel.c、accel_proto.h)。
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
#include "accel_proto.h"
#include "accel.h"

#define NETPIPE_PORT    6868
#define NETPIPE_MAGIC   0x58364e50      /* "X6NP" (big endian) */
#define NETPIPE_MAGIC_R 0x58364e52      /* "X6NR": レコードで運ぶ (ブリッジだけ) */
#define REC_HDR         16              /* Z へのレコードの見出し (長さ 2 + 空き 14) */
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
static volatile bool raw;               /* レコードで運ぶ ("X6NR") */
static struct netif *wifi;
static netif_input_fn wifi_input;       /* 横取りする前の netif->input */
static volatile bool arp_pending;       /* Z 宛ての ARP 要求に答える (USB タスクで送る) */
static struct eth_addr arp_mac;
static ip4_addr_t arp_ip;
static bool announced;                  /* Z の IP アドレスを名乗った (gratuitous ARP) */
/* Z 宛ての ping (ICMP echo request) は Z に渡さず、Pico が代わりに返す (代返)。ESP/X は TCP を閉じたあとに
   外から echo request を受けると止まった。返すのは Z が次に交換しに来たとき (USB タスク) なので、
   Z が固まっていれば返事は出ない (ping で Z が生きているかは分かる) */
static uint8_t echo_pkt[MTU];
static volatile uint16_t echo_len;      /* 0 でなければ返す echo request が入っている */

extern volatile int sysstatus;

/* 16/32 ビットの値は X68000 側に合わせてビッグエンディアン */
static uint16_t be16(uint16_t v) { return (v >> 8) | (v << 8); }
static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

/* リングの空き。足りなければ false */
static bool ring_room(uint32_t n)
{
    return n <= RXRING - (rx_head - rx_tail);
}

/* offset から len バイトを SLIP にしてリングに積む (入りきらないときは丸ごと捨てる) */
static void ring_put_slip(const struct pbuf *p, uint16_t offset, uint16_t len)
{
    if (!ring_room(2 * len + 2)) {
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
        for (; i < q->len && len > 0; i++, len--) {
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

/* offset から len バイトを Z へのレコードにしてリングに積む (入りきらないときは捨てる) */
static void ring_put_rec(const struct pbuf *p, uint16_t offset, uint16_t len)
{
    uint32_t rl = REC_HDR + ((len + 1) & ~1);
    if (!ring_room(rl)) {
        st_drop++;
        return;
    }
    uint32_t h = rx_head;
    rxring[h++ % RXRING] = len >> 8;
    rxring[h++ % RXRING] = len;
    for (int i = 2; i < REC_HDR; i++)
        rxring[h++ % RXRING] = 0;
    uint16_t skip = offset, left = len;
    for (const struct pbuf *q = p; q != NULL && left > 0; q = q->next) {
        const uint8_t *s = q->payload;
        uint16_t i;
        if (skip >= q->len) {
            skip -= q->len;
            continue;
        }
        for (i = skip, skip = 0; i < q->len && left > 0; i++, left--)
            rxring[h++ % RXRING] = s[i];
    }
    if (len & 1)
        rxring[h++ % RXRING] = 0;
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
                /* 外からの ping は Z に渡さず、Pico が代わりに返す (echo_pkt を参照) */
                /* Ethernet の詰め物を Z に渡さないよう、IP の全長で切る */
                int tot = lwip_ntohs(IPH_LEN(ih));
                if (IPH_PROTO(ih) == IP_PROTO_ICMP && p->len >= SIZEOF_ETH_HDR + hl + 1 && ip[hl] == 8) {
                    if (echo_len == 0 && tot <= MTU && tot <= p->tot_len - SIZEOF_ETH_HDR &&
                        pbuf_copy_partial(p, echo_pkt, tot, SIZEOF_ETH_HDR) == tot) {
                        __dmb();
                        echo_len = tot;
                    } else {
                        st_drop++;
                    }
                }
                else if (tot < 20 || tot > p->tot_len - SIZEOF_ETH_HDR)
                    st_drop++;
                else if (raw)
                    ring_put_rec(p, SIZEOF_ETH_HDR, tot);
                else
                    ring_put_slip(p, SIZEOF_ETH_HDR, tot);
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

/* IP パケット 1 個を Wi-Fi に出す */
static void bridge_send(const uint8_t *pkt, int tot)
{
    struct pbuf *p = pbuf_alloc(PBUF_IP, tot, PBUF_POOL);
    if (p != NULL) {
        ip4_addr_t dst;
        pbuf_take(p, pkt, tot);
        memcpy(&dst, &pkt[16], 4);
        cyw43_arch_lwip_begin();
        etharp_output(wifi, p, &dst);   /* LAN の外ならゲートウェイの MAC で出す */
        cyw43_arch_lwip_end();
        pbuf_free(p);
    } else {
        st_drop++;
    }
}

/* 16 ビットの 1 の補数の和 (IP / ICMP のチェックサム) */
static uint16_t cksum(const uint8_t *b, int n)
{
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2)
        s += (b[i] << 8) | b[i + 1];
    if (n & 1)
        s += b[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return ~s & 0xffff;
}

/* 預かった echo request に、Z の代わりに echo reply を返す (USB タスクで、lwIP のロックの外から呼ぶ) */
static void echo_reply(void)
{
    int tot = echo_len;
    if (tot == 0)
        return;
    uint8_t *ip = echo_pkt;
    int hl = (ip[0] & 15) * 4;
    if (hl >= 20 && tot >= hl + 8) {
        uint8_t t[4];
        memcpy(t, &ip[12], 4);           /* 送り元と宛先を入れ替える */
        memcpy(&ip[12], &ip[16], 4);
        memcpy(&ip[16], t, 4);
        ip[8] = 64;                      /* TTL */
        ip[10] = ip[11] = 0;
        uint16_t c = cksum(ip, hl);
        ip[10] = c >> 8;
        ip[11] = c;
        uint8_t *icmp = ip + hl;
        icmp[0] = 0;                     /* echo reply */
        icmp[2] = icmp[3] = 0;
        c = cksum(icmp, tot - hl);
        icmp[2] = c >> 8;
        icmp[3] = c;
        bridge_send(ip, tot);
    }
    __dmb();
    echo_len = 0;
}

/* Z から来たレコード ([長さ 2][IP パケット][詰め物]) を 1 個ずつ Wi-Fi に出す */
static void bridge_out_rec(const uint8_t *s, int n)
{
    while (n >= 2) {
        int len = (s[0] << 8) | s[1];
        int rl = 2 + ((len + 1) & ~1);
        if (len == 0 || rl > n)
            break;
        const uint8_t *pkt = s + 2;
        /* xip.x の詰め物 (Ethernet の最小長) があるので、全長 <= 長さ のものを全長の分だけ出す */
        int tot = len >= 20 ? (pkt[2] << 8) | pkt[3] : 0;
        if (pkt[0] == ACCEL_MARK)
            accel_submit(pkt, len);     /* 暗号アクセラレータの要求 (IPv4 のバージョンと重ならない) */
        else if ((pkt[0] >> 4) == 4 && tot >= 20 && tot <= len)
            bridge_send(pkt, tot);
        s += rl;
        n -= rl;
    }
}

/* Z から来た SLIP をほどき、1 パケットそろうたびに Wi-Fi に出す */
static void bridge_out(const uint8_t *s, int n)
{
    static uint8_t pkt[MTU];
    static int pos, esc, bad;
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == SLIP_END) {
            /* IPv4 で、全長 <= 受け取った長さ のものを全長の分だけ出す。xip.x は Ethernet の最小長 (60 バイト) まで
               詰め物を足し、slip.sys はそれごと送ってくるので、小さいパケット (SYN-ACK など) は全長より長い */
            int tot = pos >= 20 ? (pkt[2] << 8) | pkt[3] : 0;
            if (pos >= 20 && !bad && (pkt[0] >> 4) == 4 && tot >= 20 && tot <= pos)
                bridge_send(pkt, tot);
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
    accel_init();
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
    bool r = z != 0 && rq->magic == be32(NETPIPE_MAGIC_R);
    int n = 0;

    if (np_open()) {
        if (z != zip || r != raw) {
            zip = z;
            raw = r;
            announced = false;
            rx_tail = rx_head;          /* 運び方が変わったら残りは捨てる */
            if (z != 0)
                printf("netpipe: bridge %d.%d.%d.%d%s\n", z & 255, (z >> 8) & 255, (z >> 16) & 255, z >> 24,
                       r ? " (records)" : "");
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
            echo_reply();
            if (txlen > 0 && raw)
                bridge_out_rec(reqbuf + 16, txlen);
            else if (txlen > 0)
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
        if (raw) {
            /* 暗号アクセラレータの応答があれば先頭に載せる (最初のセクタに入る) */
            int al = accel_result(resbuf + 16 + REC_HDR);
            if (al > 0) {
                resbuf[16] = al >> 8;
                resbuf[17] = al;
                memset(resbuf + 18, 0, REC_HDR - 2);
                n = REC_HDR + ((al + 1) & ~1);
            }
            /* レコードを途中で切らない */
            while (avail >= REC_HDR) {
                int len = (rxring[t % RXRING] << 8) | rxring[(t + 1) % RXRING];
                uint32_t rl = REC_HDR + ((len + 1) & ~1);
                if (rl > avail || n + rl > (uint32_t)rxmax)
                    break;
                for (uint32_t i = 0; i < rl; i++)
                    resbuf[16 + n + i] = rxring[t++ % RXRING];
                n += rl;
                avail -= rl;
            }
        } else {
            n = avail < rxmax ? avail : rxmax;
            for (int i = 0; i < n; i++)
                resbuf[16 + i] = rxring[t++ % RXRING];
        }
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
        if ((rq->magic != be32(NETPIPE_MAGIC) && rq->magic != be32(NETPIPE_MAGIC_R)) || be16(rq->len) > NETPIPE_DATAMAX)
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
