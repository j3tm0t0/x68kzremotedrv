/*
 * tsp_mdns.c -- 名前が ".local" で終わるときに mDNS で引く (tsp_svc.c の名前解決から呼ぶ)
 *
 * 224.0.0.251:5353 に A の問い合わせを送る。送り元のポートは 5353 ではないので、答える側 (avahi、macOS など) は
 * RFC 6762 §6.7 の「従来の単発の問い合わせ」として、送り元にユニキャストで答える。だから 224.0.0.251 のグループに
 * 入らなくても答えを受け取れる。1 秒ごとに 3 回送り、答えが無ければ見つからないとする。
 * 呼び出しと戻りの形は lwIP の dns_gethostbyname と同じ (ERR_INPROGRESS のあとで cb(name, ip か NULL, arg))。
 * lwIP の中 (tcpip のスレッドか、ロックを取った中) から呼ぶ。
 */
#include <string.h>
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "lwip/ip_addr.h"
#include "tsp_mdns.h"

#define MDNS_PORT       5353
#define NPEND           4
#define TRIES           3
#define RETRY_MS        1000
#define NAME_MAX_       63

static struct udp_pcb *pcb;
static struct pend {
  char name[NAME_MAX_ + 1];
  dns_found_callback cb;
  void *arg;
  u16_t id;
  u8_t tries;
  u8_t used;
} pend[NPEND];
static u16_t next_id;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int tsp_mdns_is_local(const char *name)
{
  size_t n = strlen(name);
  const char *s = ".local";
  if (n > 0 && name[n - 1] == '.')      /* 末尾の "." は付いていてもよい */
    n--;
  if (n <= 6)
    return 0;
  for (int i = 0; i < 6; i++)
    if (lower((unsigned char)name[n - 6 + i]) != s[i])
      return 0;
  return 1;
}

/* 問い合わせのパケット: 見出し 12 + 名前 (ラベルの列) + 型 A + クラス IN */
static struct pbuf *build(const struct pend *q)
{
  u8_t b[12 + NAME_MAX_ + 2 + 4];
  int n = 12;
  const char *s = q->name;
  memset(b, 0, 12);
  b[0] = q->id >> 8;
  b[1] = q->id;
  b[5] = 1;                             /* QDCOUNT */
  while (*s) {
    const char *d = strchr(s, '.');
    int l = d ? (int)(d - s) : (int)strlen(s);
    if (l == 0 || l > 63 || n + 1 + l + 5 > (int)sizeof b)
      return NULL;
    b[n++] = l;
    memcpy(b + n, s, l);
    n += l;
    s += l;
    if (*s == '.')
      s++;
  }
  b[n++] = 0;
  b[n++] = 0; b[n++] = 1;               /* A */
  b[n++] = 0; b[n++] = 1;               /* IN */
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
  if (p)
    pbuf_take(p, b, n);
  return p;
}

static void finish(int k, const ip_addr_t *ip)
{
  struct pend q = pend[k];
  pend[k].used = 0;
  q.cb(q.name, ip, q.arg);
}

static void send_one(int k);

static void tick(void *arg)
{
  int k = (int)(uintptr_t)arg;
  if (!pend[k].used)
    return;
  if (pend[k].tries >= TRIES) {
    finish(k, NULL);
    return;
  }
  send_one(k);
}

static void send_one(int k)
{
  ip_addr_t grp;
  struct pbuf *p = build(&pend[k]);
  IP_ADDR4(&grp, 224, 0, 0, 251);
  pend[k].tries++;
  if (p) {
    udp_sendto(pcb, p, &grp, MDNS_PORT);
    pbuf_free(p);
  }
  sys_timeout(RETRY_MS, tick, (void *)(uintptr_t)k);
}

/* 圧縮された名前を読んで "a.b.local" の形で out に。読み終えた位置 (名前の後ろ) を返す。壊れていれば -1 */
static int read_name(const u8_t *m, int len, int off, char *out, int outsz)
{
  int end = -1, o = 0, jumps = 0;
  while (off < len) {
    int l = m[off];
    if (l == 0) {
      if (end < 0)
        end = off + 1;
      if (o > 0)
        o--;                            /* 最後の "." を消す */
      out[o] = 0;
      return end;
    }
    if ((l & 0xc0) == 0xc0) {
      if (off + 1 >= len || ++jumps > 8)
        return -1;
      if (end < 0)
        end = off + 2;
      off = ((l & 0x3f) << 8) | m[off + 1];
      continue;
    }
    if (off + 1 + l > len || o + l + 1 >= outsz)
      return -1;
    memcpy(out + o, m + off + 1, l);
    o += l;
    out[o++] = '.';
    off += 1 + l;
  }
  return -1;
}

static int same_name(const char *a, const char *b)
{
  size_t na = strlen(a), nb = strlen(b);
  if (na > 0 && a[na - 1] == '.') na--;
  if (nb > 0 && b[nb - 1] == '.') nb--;
  if (na != nb)
    return 0;
  for (size_t i = 0; i < na; i++)
    if (lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
      return 0;
  return 1;
}

static void on_recv(void *arg, struct udp_pcb *u, struct pbuf *p, const ip_addr_t *from, u16_t port)
{
  u8_t m[512];
  char nm[NAME_MAX_ + 2];
  int len, off, qd, an;
  (void)arg; (void)u; (void)from; (void)port;
  len = pbuf_copy_partial(p, m, sizeof m, 0);
  pbuf_free(p);
  if (len < 12 || !(m[2] & 0x80))        /* 答え (QR) だけ */
    return;
  u16_t id = (m[0] << 8) | m[1];
  int k;
  for (k = 0; k < NPEND; k++)
    if (pend[k].used && pend[k].id == id)
      break;
  if (k == NPEND)
    return;
  qd = (m[4] << 8) | m[5];
  an = (m[6] << 8) | m[7];
  off = 12;
  while (qd-- > 0) {
    if ((off = read_name(m, len, off, nm, sizeof nm)) < 0 || off + 4 > len)
      return;
    off += 4;
  }
  while (an-- > 0) {
    int type, cls, rdl;
    if ((off = read_name(m, len, off, nm, sizeof nm)) < 0 || off + 10 > len)
      return;
    type = (m[off] << 8) | m[off + 1];
    cls = ((m[off + 2] << 8) | m[off + 3]) & 0x7fff;   /* 上の 1 ビットは「キャッシュを入れ替える」 */
    rdl = (m[off + 8] << 8) | m[off + 9];
    off += 10;
    if (off + rdl > len)
      return;
    if (type == 1 && cls == 1 && rdl == 4 && same_name(nm, pend[k].name)) {
      ip_addr_t ip;
      IP_ADDR4(&ip, m[off], m[off + 1], m[off + 2], m[off + 3]);
      sys_untimeout(tick, (void *)(uintptr_t)k);
      finish(k, &ip);
      return;
    }
    off += rdl;
  }
}

err_t tsp_mdns_lookup(const char *name, dns_found_callback cb, void *arg)
{
  int k;
  size_t n = strlen(name);
  if (n > NAME_MAX_)
    return ERR_ARG;
  if (!pcb) {
    if (!(pcb = udp_new_ip_type(IPADDR_TYPE_V4)))
      return ERR_MEM;
    if (udp_bind(pcb, IP4_ADDR_ANY, 0) != ERR_OK) {
      udp_remove(pcb);
      pcb = NULL;
      return ERR_MEM;
    }
    udp_recv(pcb, on_recv, NULL);
  }
  for (k = 0; k < NPEND; k++)
    if (!pend[k].used)
      break;
  if (k == NPEND)
    return ERR_MEM;
  memcpy(pend[k].name, name, n + 1);
  if (n > 0 && pend[k].name[n - 1] == '.')
    pend[k].name[n - 1] = 0;
  pend[k].cb = cb;
  pend[k].arg = arg;
  pend[k].id = ++next_id ? next_id : ++next_id;
  pend[k].tries = 0;
  pend[k].used = 1;
  send_one(k);
  return ERR_INPROGRESS;
}

/* ".local" なら mDNS、ほかは lwIP の DNS */
err_t tsp_gethostbyname(const char *name, ip_addr_t *ip, dns_found_callback cb, void *arg)
{
  if (tsp_mdns_is_local(name))
    return tsp_mdns_lookup(name, cb, arg);
  return dns_gethostbyname(name, ip, cb, arg);
}
