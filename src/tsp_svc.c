/*
 * tsp_svc.c -- Pico のソケット窓口: tis.x (X68000 Z) のソケット操作を lwIP で行う
 *
 * 記録形式は ../tsp.h。Z の要求 (CONNECT / LISTEN / DATA / WINDOW / CLOSE ...) を lwIP の raw API に写し、
 * lwIP のコールバックが残した状態 (つながった・受けたデータ・FIN・エラー・accept) を次の応答にまとめる。
 * 交換の外 (lwIP の文脈) で起きたことは表に印を付けるだけで、Z へ送るのは常に tsp_exchange() の中。
 *
 * フロー制御 (README「Pico のソケット窓口」):
 *   Pico → Z: Z の受信リングの空き (WINDOW) の分だけ送る。受けた pbuf は Z に渡すまで持ち、渡した分だけ
 *             tcp_recved する。TCP の窓が Z の受信リングにつながり、Pico が持つのはソケットあたり TCP_WND まで
 *   Z → Pico: 応答のたびに CREDIT (次の要求で送ってよい量、絶対値) を知らせる。tcp_sndbuf と送信キューの空き、
 *             全体の上限 TSP_TXPOOL を超えない。Z は 1 要求で 1 ソケットにつき DATA を 1 個しか送らないので、
 *             tcp_write 1 回分の pbuf (ceil(n / mss) + 1) が入る量にする
 *   公平さ: データは毎回始まりの番号を回しながら、送れるソケットで等分してから余りを詰める
 */
#include <string.h>
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "tsp.h"
#include "tsp_svc.h"
#include "tsp_plat.h"

#ifndef TSP_NPS
#define TSP_NPS         40
#endif
#define NPS             TSP_NPS         /* Pico 側のソケット (Z の 32 番号 + accept されて引き取り待ちの子) */
#define NONE            0xff
#define UDPQ            4               /* UDP: Z に渡していない受信の最大 (古いものから捨てる) */
#ifndef TSP_TXPOOL
#define TSP_TXPOOL      (12 * 1024)     /* lwIP の送信キューに入れてよい合計 (CREDIT の上限)。MEM_SIZE に収める */
#endif
#define CREDIT_FLOOR    1024            /* 送るものを申告していないソケットにも渡しておく量 (打鍵がすぐ出るように) */

enum { P_FREE, P_DNS, P_CONNECTING, P_EST, P_LISTEN, P_UDP, P_DEAD };

#define EV_CONNECTED    0x01
#define EV_BOUND        0x02
#define EV_EOF          0x04            /* FIN を受けた (データを渡し切ってから EOF を送る) */
#define EV_ERROR        0x08            /* pcb が無くなった (データを渡し切ってから ERROR を送る) */
#define EV_EOF_SENT     0x10
#define EV_ANNOUNCE     0x20            /* 子: listen 側に ACCEPT を送る */

struct psock {
  uint8_t state, ev, err;
  uint8_t zslot, zgen;                  /* Z の番号と gen (NONE = まだ引き取られていない子) */
  uint8_t listener;                     /* 子: 親の listen (NONE = 引き取られた) */
  uint8_t backlog, npend;               /* listen: 引き取り待ちの子の数 */
  uint8_t serial;                       /* DNS のコールバックが古いソケットに当たらないように */
  uint8_t flags;
  uint16_t lport, rport;
  struct tcp_pcb *tcp;
  struct udp_pcb *udp;
  struct pbuf *rxq;                     /* TCP: Z に渡していない受信 */
  struct { struct pbuf *p; uint32_t addr; uint16_t port; } uq[UDPQ];
  uint8_t uqh, uqn;
  uint32_t zwin;                        /* Z の受信リングの残り */
  uint32_t zcred;                       /* Z が知っている CREDIT の残り */
  uint32_t want;                        /* Z の送信リングに残っている量 (WINDOW で申告) */
};

static struct psock ps[NPS];
static uint8_t z2p[TSP_NSLOT];

static struct { uint8_t state, code, serial; uint32_t addr; } q[TSP_NQUERY];   /* state: 0 空き / 1 引いている / 2 答え待ち */
static uint8_t qserial;

static uint32_t zepoch, pepoch;
static uint16_t last_zseq;
static uint8_t have_last;
static uint8_t last[TSP_RES_MAX];       /* 最後の応答 (Z が同じ要求を送り直したら、これをそのまま返す) */
static int lastlen, lastmore;
static uint8_t rr;

struct tsp_stat tsp_stat;

static uint32_t ip4(const ip_addr_t *a)
{
  return lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(a)));
}

static void set_ip4(ip_addr_t *a, uint32_t v)
{
  ip_addr_set_ip4_u32(a, lwip_htonl(v));
}

static int ps_alloc(void)
{
  int i;
  for (i = 0; i < NPS; i++)
    if (ps[i].state == P_FREE) {
      uint8_t serial = ps[i].serial + 1;
      memset(&ps[i], 0, sizeof ps[i]);
      ps[i].serial = serial;
      ps[i].zslot = ps[i].listener = NONE;
      return i;
    }
  return -1;
}

static struct psock *by_slot(int slot, int gen)
{
  int k;
  if (slot >= TSP_NSLOT || (k = z2p[slot]) == NONE || ps[k].zgen != gen)
    return 0;
  return &ps[k];
}

/* ---- lwIP のコールバック (交換の外) ---- */

static err_t cb_recv_closed(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  (void)arg; (void)err;
  if (p) {
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
  }
  return ERR_OK;
}

static void detach(struct tcp_pcb *pcb)
{
  tcp_arg(pcb, NULL);
  tcp_recv(pcb, cb_recv_closed);
  tcp_sent(pcb, NULL);
  tcp_err(pcb, NULL);
  tcp_poll(pcb, NULL, 0);
}

static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  struct psock *s = arg;
  (void)err;
  if (!s)
    return cb_recv_closed(arg, pcb, p, err);
  if (!p) {
    s->ev |= EV_EOF;
    return ERR_OK;
  }
  if (s->rxq)
    pbuf_cat(s->rxq, p);
  else
    s->rxq = p;
  return ERR_OK;
}

static void cb_err(void *arg, err_t err)
{
  struct psock *s = arg;
  if (!s)
    return;
  s->tcp = NULL;                        /* lwIP はもう pcb を解放した */
  if (s->state == P_CONNECTING)
    s->err = err == ERR_RST ? TSP_ERR_REFUSED : TSP_ERR_TIMEOUT;
  else
    s->err = err == ERR_RST ? TSP_ERR_RESET : TSP_ERR_ABORTED;
  if (s->err == TSP_ERR_REFUSED)
    tsp_stat.refused++;
  s->state = P_DEAD;
  s->ev |= EV_ERROR;
}

static err_t cb_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
  struct psock *s = arg;
  (void)pcb; (void)err;
  if (s && s->state == P_CONNECTING) {
    s->state = P_EST;
    s->ev |= EV_CONNECTED;
  }
  return ERR_OK;
}

static void set_tcp(struct psock *s, struct tcp_pcb *pcb)
{
  s->tcp = pcb;
  tcp_arg(pcb, s);
  tcp_recv(pcb, cb_recv);
  tcp_err(pcb, cb_err);
  tcp_nagle_disable(pcb);               /* Z は 1 交換ぶんまとめて送るので、Nagle で待つ理由が無い */
}

static err_t cb_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
  struct psock *l = arg;
  int c;
  if (err != ERR_OK || !pcb)
    return ERR_VAL;
  if (!l || l->state != P_LISTEN || l->npend >= (l->backlog ? l->backlog : 1) || (c = ps_alloc()) < 0) {
    tcp_abort(pcb);
    return ERR_ABRT;
  }
  ps[c].state = P_EST;
  ps[c].listener = l - ps;
  ps[c].ev = EV_ANNOUNCE;
  set_tcp(&ps[c], pcb);
  l->npend++;
  tsp_stat.accepts++;
  return ERR_OK;
}

static void cb_udp(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
  struct psock *s = arg;
  int k;
  (void)pcb;
  if (!s || s->state != P_UDP) {
    pbuf_free(p);
    return;
  }
  if (s->uqn == UDPQ) {
    pbuf_free(s->uq[s->uqh].p);
    s->uqh = (s->uqh + 1) % UDPQ;
    s->uqn--;
    tsp_stat.udp_drop++;
  }
  k = (s->uqh + s->uqn) % UDPQ;
  s->uq[k].p = p;
  s->uq[k].addr = ip4(addr);
  s->uq[k].port = port;
  s->uqn++;
}

static void start_connect(struct psock *s, const ip_addr_t *ip);

static void cb_dns_connect(const char *name, const ip_addr_t *ip, void *arg)
{
  uintptr_t v = (uintptr_t)arg;
  struct psock *s = &ps[v & 0xff];
  (void)name;
  if (s->state != P_DNS || s->serial != (uint8_t)(v >> 8))
    return;
  if (!ip) {
    s->state = P_DEAD;
    s->err = TSP_ERR_NONAME;
    s->ev |= EV_ERROR;
    return;
  }
  start_connect(s, ip);
}

static void cb_dns_query(const char *name, const ip_addr_t *ip, void *arg)
{
  uintptr_t v = (uintptr_t)arg;
  int k = v & 0xff;
  (void)name;
  if (q[k].state != 1 || q[k].serial != (uint8_t)(v >> 8))
    return;
  q[k].state = 2;
  q[k].code = ip ? TSP_OK : TSP_ERR_NONAME;
  q[k].addr = ip ? ip4(ip) : 0;
}

/* ---- 後始末 ---- */

static void free_rx(struct psock *s)
{
  if (s->rxq)
    pbuf_free(s->rxq);
  s->rxq = NULL;
  while (s->uqn) {
    pbuf_free(s->uq[s->uqh].p);
    s->uqh = (s->uqh + 1) % UDPQ;
    s->uqn--;
  }
}

static void unmap(struct psock *s)
{
  if (s->zslot != NONE && z2p[s->zslot] == s - ps)
    z2p[s->zslot] = NONE;
  s->zslot = NONE;
}

/* Pico 側のソケットを捨てる。graceful: TCP は残りを送ってから FIN (相手のデータを読み残していれば lwIP が RST) */
static void ps_close(struct psock *s, int graceful)
{
  int i;
  if (s->state == P_LISTEN) {
    /* 引き取られていない子も切る */
    for (i = 0; i < NPS; i++)
      if (ps[i].state != P_FREE && ps[i].listener == s - ps)
        ps_close(&ps[i], 0);
  }
  if (s->listener != NONE && ps[s->listener].npend)
    ps[s->listener].npend--;
  if (s->tcp && s->state == P_LISTEN) {
    tcp_arg(s->tcp, NULL);              /* listen の pcb には recv / err を付けられない */
    tcp_accept(s->tcp, NULL);
    tcp_close(s->tcp);
    s->tcp = NULL;
  } else if (s->tcp) {
    detach(s->tcp);
    if (!graceful || tcp_close(s->tcp) != ERR_OK)
      tcp_abort(s->tcp);
    s->tcp = NULL;
  }
  if (s->udp) {
    udp_remove(s->udp);
    s->udp = NULL;
  }
  free_rx(s);
  unmap(s);
  s->listener = NONE;
  s->state = P_FREE;
}

static void abort_all(void)
{
  int i;
  for (i = 0; i < NPS; i++)
    if (ps[i].state != P_FREE) {
      ps_close(&ps[i], 0);
      tsp_stat.aborts++;
    }
  for (i = 0; i < TSP_NQUERY; i++)
    q[i].state = 0;
  memset(z2p, NONE, sizeof z2p);
}

/* ---- 要求 ---- */

static void fail(struct psock *s, int code)
{
  if (s->tcp) {
    detach(s->tcp);
    tcp_abort(s->tcp);
    s->tcp = NULL;
  }
  s->state = P_DEAD;
  s->err = code;
  s->ev |= EV_ERROR;
}

static void start_connect(struct psock *s, const ip_addr_t *ip)
{
  struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
  err_t e;
  if (!pcb) {
    fail(s, TSP_ERR_NOMEM);
    return;
  }
  set_tcp(s, pcb);
  s->state = P_CONNECTING;
  e = tcp_connect(pcb, ip, s->rport, cb_connected);
  if (e != ERR_OK)
    fail(s, e == ERR_RTE ? TSP_ERR_UNREACH : TSP_ERR_NOMEM);
  tsp_stat.connects++;
}

/* 新しいソケットを Z の番号に結ぶ (前のものが残っていれば捨てる: 閉じた知らせが届かなかった) */
static struct psock *attach(int slot, int gen)
{
  int k;
  if (slot >= TSP_NSLOT)
    return 0;
  if (z2p[slot] != NONE)
    ps_close(&ps[z2p[slot]], 0);
  if ((k = ps_alloc()) < 0)
    return 0;
  ps[k].zslot = slot;
  ps[k].zgen = gen;
  z2p[slot] = k;
  return &ps[k];
}

static void c_connect(int slot, int gen, const uint8_t *b, unsigned n)
{
  struct psock *s;
  ip_addr_t ip;
  uint32_t addr;
  char name[TSP_NAME_MAX + 1];
  unsigned nl = n > 10 ? n - 10 : 0;
  if (n < 10 || !(s = attach(slot, gen)))
    return;
  s->rport = tsp_get16(b + 4);
  addr = tsp_get32(b + 6);
  if (b[0] != TSP_PROTO_TCP || (b[1] & TSP_CF_TLS)) {
    s->state = P_DEAD;                  /* TLS はまだ (README の「TLS を足すとき」) */
    s->err = TSP_ERR_NOTSUP;
    s->ev |= EV_ERROR;
    return;
  }
  if (addr == 0 && (b[1] & TSP_CF_NAME) && nl) {
    /* 名前でつなぐ: Pico が引く (IPv6 しか無い名前もここで扱える) */
    err_t e;
    if (nl > TSP_NAME_MAX)
      nl = TSP_NAME_MAX;
    memcpy(name, b + 10, nl);
    name[nl] = 0;
    s->state = P_DNS;
    tsp_stat.dns++;
    e = dns_gethostbyname(name, &ip, cb_dns_connect, (void *)(uintptr_t)((s - ps) | (s->serial << 8)));
    if (e == ERR_OK)
      start_connect(s, &ip);
    else if (e != ERR_INPROGRESS)
      fail(s, TSP_ERR_NONAME);
    return;
  }
  set_ip4(&ip, addr);
  start_connect(s, &ip);
}

static void c_listen(int slot, int gen, const uint8_t *b, unsigned n)
{
  struct psock *s;
  struct tcp_pcb *pcb, *lp;
  ip_addr_t ip;
  if (n < 8 || !(s = attach(slot, gen)))
    return;
  if (!(pcb = tcp_new_ip_type(IPADDR_TYPE_ANY))) {
    fail(s, TSP_ERR_NOMEM);
    return;
  }
  set_ip4(&ip, tsp_get32(b + 4));
  ip_set_option(pcb, SOF_REUSEADDR);    /* 接続 1 本ごとに listen し直す rcmd.x (TIME WAIT の残りを気にしない) */
  if (tcp_bind(pcb, tsp_get32(b + 4) ? &ip : IP_ANY_TYPE, tsp_get16(b + 2)) != ERR_OK) {
    tcp_close(pcb);
    fail(s, TSP_ERR_INUSE);
    return;
  }
  if (!(lp = tcp_listen_with_backlog(pcb, TCP_DEFAULT_LISTEN_BACKLOG))) {
    tcp_close(pcb);
    fail(s, TSP_ERR_NOMEM);
    return;
  }
  s->tcp = lp;
  s->state = P_LISTEN;
  s->backlog = b[0];
  s->lport = lp->local_port;
  s->ev |= EV_BOUND;
  tcp_arg(lp, s);
  tcp_accept(lp, cb_accept);
}

static struct psock *udp_open(int slot, int gen, uint32_t laddr, uint16_t lport)
{
  struct psock *s;
  ip_addr_t ip;
  if (!(s = attach(slot, gen)))
    return 0;
  if (!(s->udp = udp_new_ip_type(IPADDR_TYPE_ANY))) {
    fail(s, TSP_ERR_NOMEM);
    return s;
  }
  set_ip4(&ip, laddr);
  if (udp_bind(s->udp, laddr ? &ip : IP_ANY_TYPE, lport) != ERR_OK) {
    udp_remove(s->udp);
    s->udp = NULL;
    fail(s, TSP_ERR_INUSE);
    return s;
  }
  s->state = P_UDP;
  s->lport = s->udp->local_port;
  s->ev |= EV_BOUND;
  udp_recv(s->udp, cb_udp, s);
  return s;
}

static void c_sendto(int slot, int gen, const uint8_t *b, unsigned n)
{
  struct psock *s = by_slot(slot, gen);
  struct pbuf *p;
  ip_addr_t ip;
  if (n < 6)
    return;
  if (!s && !(s = udp_open(slot, gen, 0, 0)))
    return;
  if (s->state != P_UDP)
    return;
  if (!(p = pbuf_alloc(PBUF_TRANSPORT, n - 6, PBUF_RAM))) {
    tsp_stat.udp_drop++;
    return;
  }
  pbuf_take(p, b + 6, n - 6);
  set_ip4(&ip, tsp_get32(b));
  udp_sendto(s->udp, p, &ip, tsp_get16(b + 4));
  pbuf_free(p);
  tsp_stat.dgrams_out++;
}

static void c_data(struct psock *s, const uint8_t *b, unsigned n)
{
  err_t e;
  if (s->state != P_EST || !s->tcp) {
    s->zcred = s->zcred > n ? s->zcred - n : 0;
    return;                             /* もう切れている (ERROR を送る / 送った) */
  }
  e = tcp_write(s->tcp, b, n, TCP_WRITE_FLAG_COPY);
  s->zcred = s->zcred > n ? s->zcred - n : 0;
  if (e != ERR_OK) {
    /* CREDIT の範囲なら起きないはず (tsp_stat.errmem を試験で見る) */
    tsp_stat.errmem++;
    TSP_LOG("tsp: tcp_write %d bytes -> %d (sndbuf %u queue %u)\n", n, e, tcp_sndbuf(s->tcp), tcp_sndqueuelen(s->tcp));
    fail(s, TSP_ERR_NOMEM);
    return;
  }
  tsp_stat.tx_bytes += n;
}

static void c_adopt(int slot, int gen, const uint8_t *b, unsigned n)
{
  int h = b[0], l;
  struct psock *c;
  if (n < 8 || h >= NPS || slot >= TSP_NSLOT)
    return;
  c = &ps[h];
  if (c->state == P_FREE || c->listener == NONE || c->zslot != NONE)
    return;
  l = c->listener;
  if (ps[l].npend)
    ps[l].npend--;
  c->listener = NONE;
  if (b[1] & TSP_AF_LISTENER) {
    /* backlog 0: listen していた番号がそのまま接続になる。listen はやめる (次の接続は断られる) */
    if (ps[l].tcp) {
      tcp_arg(ps[l].tcp, NULL);
      tcp_close(ps[l].tcp);
      ps[l].tcp = NULL;
    }
    for (n = 0; n < NPS; n++)           /* ほかの引き取り待ちの子も切る */
      if (ps[n].state != P_FREE && ps[n].listener == l)
        ps_close(&ps[n], 0);
    unmap(&ps[l]);
    ps[l].state = P_FREE;
  } else if (z2p[slot] != NONE)
    ps_close(&ps[z2p[slot]], 0);
  c->zslot = slot;
  c->zgen = gen;
  c->zwin = tsp_get32(b + 4);
  z2p[slot] = h;
}

static void c_resolve(int k, const uint8_t *b, unsigned n)
{
  char name[TSP_NAME_MAX + 1];
  ip_addr_t ip;
  err_t e;
  if (k >= TSP_NQUERY)
    return;
  if (n > TSP_NAME_MAX)
    n = TSP_NAME_MAX;
  memcpy(name, b, n);
  name[n] = 0;
  q[k].serial = ++qserial;
  q[k].state = 1;
  tsp_stat.dns++;
  e = dns_gethostbyname(name, &ip, cb_dns_query, (void *)(uintptr_t)(k | (q[k].serial << 8)));
  if (e == ERR_OK) {
    q[k].state = 2;
    q[k].code = TSP_OK;
    q[k].addr = ip4(&ip);
  } else if (e != ERR_INPROGRESS) {
    q[k].state = 2;
    q[k].code = TSP_ERR_NONAME;
  }
}

static void apply(int type, int slot, int gen, const uint8_t *b, unsigned n)
{
  struct psock *s;
  switch (type) {
  case TSP_C_CONNECT: c_connect(slot, gen, b, n); return;
  case TSP_C_LISTEN:  c_listen(slot, gen, b, n); return;
  case TSP_C_UDPOPEN:
    if (n >= 8)
      udp_open(slot, gen, tsp_get32(b + 4), tsp_get16(b + 2));
    return;
  case TSP_C_SENDTO:  c_sendto(slot, gen, b, n); return;
  case TSP_C_ADOPT:   c_adopt(slot, gen, b, n); return;
  case TSP_C_RESOLVE: c_resolve(slot, b, n); return;
  case TSP_C_REJECT:
    if (n >= 1 && b[0] < NPS && ps[b[0]].state != P_FREE && ps[b[0]].listener != NONE && ps[b[0]].zslot == NONE)
      ps_close(&ps[b[0]], 0);
    return;
  }
  if (!(s = by_slot(slot, gen))) {
    tsp_stat.badreq++;
    return;
  }
  switch (type) {
  case TSP_C_DATA:
    c_data(s, b, n);
    break;
  case TSP_C_WINDOW:
    if (n >= 8) {
      s->zwin = tsp_get32(b);
      s->want = tsp_get32(b + 4);
    }
    break;
  case TSP_C_SHUT:
    if (s->tcp && s->state == P_EST)
      tcp_shutdown(s->tcp, 0, 1);
    break;
  case TSP_C_CLOSE:
    ps_close(s, 1);
    break;
  }
}

/* ---- 応答 ---- */

static uint8_t *res_p;
static int res_room;

static uint8_t *put(int type, struct psock *s, int slot, unsigned len)
{
  uint8_t *b;
  if ((int)tsp_msize(len) > res_room)
    return 0;
  b = tsp_msg(res_p, type, s ? s->zslot : slot, s ? s->zgen : 0, len);
  if (len & 1)
    b[len] = 0;
  res_p += tsp_msize(len);
  res_room -= tsp_msize(len);
  return b;
}

/* CREDIT: 次の要求で送ってよい量。tcp_write 1 回で pbuf は ceil(n / mss) + 1 個まで使う */
static void credits(int *more)
{
  uint32_t used = 0, budget, segs = 0, g[NPS];
  int i, j;
  for (i = 0; i < NPS; i++)
    if (ps[i].tcp && ps[i].state == P_EST)
      used += TCP_SND_BUF - tcp_sndbuf(ps[i].tcp);
  if (used > tsp_stat.sndq_peak)
    tsp_stat.sndq_peak = used;
  budget = used < TSP_TXPOOL ? TSP_TXPOOL - used : 0;
  for (i = 0; i < NPS; i++)
    if (ps[i].tcp && ps[i].state == P_EST)
      segs += tcp_sndqueuelen(ps[i].tcp);
  segs = segs + 8 < MEMP_NUM_TCP_SEG ? MEMP_NUM_TCP_SEG - 8 - segs : 0;   /* lwIP 全体の TCP の segment の残り */
  /* 1 周目: 床 (CREDIT_FLOOR)、2 周目: 申告 (want) の分 */
  for (j = 0; j < 2; j++)
    for (i = 0; i < NPS; i++) {
      struct psock *s = &ps[(rr + i) % NPS];
      uint32_t cap, mss, ql, need;
      int k = s - ps;
      if (j == 0)
        g[k] = 0;
      if (s->zslot == NONE || s->state != P_EST || !s->tcp)
        continue;
      mss = tcp_mss(s->tcp);
      if (s->tcp->snd_wnd_max / 2 < mss && s->tcp->snd_wnd_max >= 2)
        mss = s->tcp->snd_wnd_max / 2;
      if (mss < 64)
        mss = 64;
      ql = tcp_sndqueuelen(s->tcp);
      cap = ql + 2 <= TCP_SND_QUEUELEN ? (TCP_SND_QUEUELEN - ql - 1) * mss : 0;
      if (cap > tcp_sndbuf(s->tcp))
        cap = tcp_sndbuf(s->tcp);
      need = j == 0 ? CREDIT_FLOOR : s->want;
      if (need > cap)
        need = cap;
      if (need <= g[k])
        continue;
      need -= g[k];
      if (need > budget)
        need = budget;
      if (need / mss + 2 > segs)
        need = segs > 2 ? (segs - 2) * mss : 0;
      g[k] += need;
      budget -= need;
      segs -= need ? need / mss + 2 : 0;
    }
  for (i = 0; i < NPS; i++) {
    struct psock *s = &ps[(rr + i) % NPS];
    int k = s - ps;
    uint8_t *b;
    if (s->zslot == NONE || s->state != P_EST || !s->tcp || g[k] == s->zcred)
      continue;
    if (!(b = put(TSP_E_CREDIT, s, 0, 4))) {
      *more = 1;
      return;
    }
    /* Z が送りたいのに足りなかったものが増えたなら、すぐ次の交換を */
    if (g[k] > s->zcred && s->want > s->zcred)
      *more = 1;
    tsp_put32(b, g[k]);
    s->zcred = g[k];
  }
}

static int build(uint8_t *res, int max)
{
  int i, j, k, ready, more = 0;
  res[0] = TSP_MARK;
  res[1] = TSP_VERSION;
  res[2] = 0;
  res[3] = 0;
  tsp_put16(res + 4, last_zseq);
  tsp_put16(res + 6, 0);
  tsp_put32(res + 8, pepoch);
  res_p = res + TSP_HDR;
  res_room = max - TSP_HDR;

  /* 1. 状態の知らせ (つながった・ポート・accept・名前) */
  for (i = 0; i < NPS; i++) {
    struct psock *s = &ps[(rr + i) % NPS];
    uint8_t *b;
    if (s->state == P_FREE)
      continue;
    if ((s->ev & EV_ANNOUNCE) && s->listener != NONE && ps[s->listener].zslot != NONE) {
      struct psock *l = &ps[s->listener];
      if (!s->tcp) {                    /* 知らせる前に切れた */
        ps_close(s, 0);
        continue;
      }
      if (!(b = put(TSP_E_ACCEPT, l, 0, 14))) {
        more = 1;
        continue;
      }
      b[0] = s - ps;
      b[1] = 0;
      tsp_put32(b + 2, ip4(&s->tcp->local_ip));
      tsp_put16(b + 6, s->tcp->local_port);
      tsp_put32(b + 8, ip4(&s->tcp->remote_ip));
      tsp_put16(b + 12, s->tcp->remote_port);
      s->ev &= ~EV_ANNOUNCE;
    }
    if (s->zslot == NONE)
      continue;
    if ((s->ev & EV_CONNECTED) && s->tcp) {
      if (!(b = put(TSP_E_CONNECTED, s, 0, 12))) {
        more = 1;
        continue;
      }
      tsp_put32(b, ip4(&s->tcp->local_ip));
      tsp_put16(b + 4, s->tcp->local_port);
      tsp_put32(b + 6, ip4(&s->tcp->remote_ip));
      tsp_put16(b + 10, s->tcp->remote_port);
      s->ev &= ~EV_CONNECTED;
    }
    if (s->ev & EV_BOUND) {
      if (!(b = put(TSP_E_BOUND, s, 0, 2))) {
        more = 1;
        continue;
      }
      tsp_put16(b, s->lport);
      s->ev &= ~EV_BOUND;
    }
  }
  for (k = 0; k < TSP_NQUERY; k++)
    if (q[k].state == 2) {
      uint8_t *b = put(TSP_E_RESOLVED, 0, k, 6);
      if (!b) {
        more = 1;
        break;
      }
      b[0] = q[k].code;
      b[1] = 0;
      tsp_put32(b + 2, q[k].addr);
      q[k].state = 0;
    }

  /* 2. CREDIT */
  credits(&more);

  /* 3. データ: Z の窓がある受信を、ソケットで等分してから余りを詰める */
  for (ready = 0, i = 0; i < NPS; i++)
    if (ps[i].zslot != NONE && ps[i].zwin && (ps[i].rxq || ps[i].uqn))
      ready++;
  if (ready) {
    int share = res_room / ready, pass;
    if (share < 512)
      share = 512;
    for (pass = 0; pass < 2; pass++)
      for (j = 0; j < NPS; j++) {
        struct psock *s = &ps[(rr + j) % NPS];
        uint32_t n;
        uint8_t *b;
        if (s->zslot == NONE || !s->zwin)
          continue;
        while (s->uqn) {
          /* UDP: 1 個ずつ。Z のリングには [長さ 2][ポート 2][アドレス 4] + データで入る */
          struct pbuf *p = s->uq[s->uqh].p;
          if (p->tot_len + 8 > s->zwin)
            break;
          if (!(b = put(TSP_E_DGRAM, s, 0, 6 + p->tot_len))) {
            more = 1;
            break;
          }
          tsp_put32(b, s->uq[s->uqh].addr);
          tsp_put16(b + 4, s->uq[s->uqh].port);
          pbuf_copy_partial(p, b + 6, p->tot_len, 0);
          s->zwin -= p->tot_len + 8;
          pbuf_free(p);
          s->uqh = (s->uqh + 1) % UDPQ;
          s->uqn--;
          tsp_stat.dgrams_in++;
        }
        if (!s->rxq || res_room <= TSP_MHDR + 1)
          continue;
        n = s->rxq->tot_len;
        if (n > s->zwin)
          n = s->zwin;
        if (n > (uint32_t)res_room - TSP_MHDR)
          n = res_room - TSP_MHDR;
        if (pass == 0 && n > (uint32_t)share)
          n = share;
        if (n > 1)
          n &= ~1u;
        if (!(b = put(TSP_E_DATA, s, 0, n)))
          continue;
        pbuf_copy_partial(s->rxq, b, n, 0);
        s->rxq = pbuf_free_header(s->rxq, n);
        if (s->tcp)
          tcp_recved(s->tcp, n);
        s->zwin -= n;
        tsp_stat.rx_bytes += n;
      }
  }

  /* 4. 渡し切ったソケットの EOF / ERROR */
  for (i = 0; i < NPS; i++) {
    struct psock *s = &ps[i];
    if (s->state == P_FREE || s->zslot == NONE)
      continue;
    if (s->rxq) {
      if (s->zwin)
        more = 1;
      continue;
    }
    if ((s->ev & (EV_EOF | EV_EOF_SENT)) == EV_EOF) {
      if (!put(TSP_E_EOF, s, 0, 0)) {
        more = 1;
        continue;
      }
      s->ev |= EV_EOF_SENT;
    }
    if (s->ev & EV_ERROR) {
      uint8_t *b = put(TSP_E_ERROR, s, 0, 1);
      if (!b) {
        more = 1;
        continue;
      }
      b[0] = s->err;
      ps_close(s, 0);                   /* Z の番号から外す。Z は CLOSE を送らない */
    }
  }
  for (i = 0; i < NPS; i++)
    if (ps[i].zslot != NONE && ps[i].uqn && ps[i].zwin >= ps[i].uq[ps[i].uqh].p->tot_len + 8u)
      more = 1;
  {
    uint32_t held = 0;
    for (i = 0; i < NPS; i++)
      if (ps[i].rxq)
        held += ps[i].rxq->tot_len;
    if (held > tsp_stat.held_peak)
      tsp_stat.held_peak = held;
  }
  rr++;
  if (more)
    res[2] |= TSP_PF_MORE;
  return res_p - res;
}

/* ---- 入口 ---- */

/* Wi-Fi が切れたとき / 再起動の前に: 全部のソケットを捨てる (Z には次の交換で何も知らせない。epoch を変えて
   tsp_init し直せば、Z は "Network is down" にする) */
int tsp_nsock(void)
{
  int i, n = 0;
  for (i = 0; i < NPS; i++)
    if (ps[i].state != P_FREE)
      n++;
  return n;
}

void tsp_shutdown(void)
{
  TSP_LOCK();
  abort_all();
  TSP_UNLOCK();
}

void tsp_init(uint32_t epoch)
{
  TSP_LOCK();
  memset(ps, 0, sizeof ps);
  memset(z2p, NONE, sizeof z2p);
  memset(q, 0, sizeof q);
  memset(&tsp_stat, 0, sizeof tsp_stat);
  pepoch = epoch;
  zepoch = 0;
  have_last = 0;
  rr = 0;
  TSP_UNLOCK();
}

int tsp_exchange(const uint8_t *req, int reqlen, uint8_t *res, int resmax, int *more)
{
  const uint8_t *p, *e;
  uint32_t ze;
  uint16_t zs;
  int i;
  *more = 0;
  if (reqlen < TSP_HDR || req[0] != TSP_MARK || req[1] != TSP_VERSION) {
    tsp_stat.badreq++;
    return 0;
  }
  if (resmax > TSP_RES_MAX)
    resmax = TSP_RES_MAX;
  zs = tsp_get16(req + 4);
  ze = tsp_get32(req + 8);
  TSP_LOCK();
  if (have_last && ze == zepoch && zs == last_zseq && lastlen <= resmax) {
    /* Z が同じ要求を送り直した (前の応答を受け取れなかった): 適用せずに同じ応答を返す */
    memcpy(res, last, lastlen);
    *more = lastmore;
    tsp_stat.dup++;
    TSP_UNLOCK();
    return lastlen;
  }
  if (ze != zepoch || (req[2] & TSP_ZF_RESET)) {
    /* tis.x が常駐し直した (Z のリセット) / -r: 前のソケットは全部捨てる */
    if (zepoch)
      TSP_LOG("tsp: new Z epoch %08lx, closing all sockets\n", (unsigned long)ze);
    abort_all();
    zepoch = ze;
  }
  tsp_stat.xchg++;
  last_zseq = zs;
  for (p = req + TSP_HDR, e = req + reqlen; p + TSP_MHDR <= e; ) {
    unsigned n = tsp_get16(p + 4);
    if (p + TSP_MHDR + n > e) {
      tsp_stat.badreq++;
      break;
    }
    apply(p[0], p[1], p[2], p + TSP_MHDR, n);
    p += tsp_msize(n);
  }
  for (i = 0; i < NPS; i++)
    if (ps[i].tcp && ps[i].state == P_EST)
      tcp_output(ps[i].tcp);
  lastlen = build(last, resmax);
  lastmore = (last[2] & TSP_PF_MORE) != 0;
  have_last = 1;
  memcpy(res, last, lastlen);
  *more = lastmore;
  TSP_UNLOCK();
  return lastlen;
}
