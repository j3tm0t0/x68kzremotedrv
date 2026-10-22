/*
 * bootwait.c -- HDS が開けるまで Z の画面に接続待ちを出して待たせる (BOOTWAIT_SCREEN)
 *
 * Z の Linux は HDS を読み始めるとき (電源投入から約 35 秒) に LBA 0-31 (16KB: 署名・IPL・分割表・SCSI ドライバ) を
 * まとめて読み、ソフトリセットのあとも読み直さずキャッシュから起動する。だから Wi-Fi / SMB が間に合わないとき
 * そこに偽物を返すと、分割表とドライバが偽物のまま電源を切るまで残る。そこで:
 *
 *   1. HDS を開けたら LBA 0-31 を控え (フラッシュ BW_FLASH_OFFSET) と比べ、違えば (HDS のパスと大きさも) 控え直す。
 *   2. HDS がまだ開けていない間は、LBA 0-31 を控えのとおりに返す。ただし IPL (LBA 2-3) は待ち IPL の前半
 *      (driver/bootwait1.S、後半 bootwait.S は ID 6 の BW_STAGE2) にする (開けているときに hdsboot に差し替えている
 *      のと同じ場所)。控えが無ければ
 *      0014 と同じく NOT READY で待たせる (偽物は返さない)。
 *   3. 待ち IPL は画面に状態を出しながら ID 6 の BW_BAND を読みに来る (bootwait_proto.h)。開けて控えと同じなら
 *      本物の IPL を渡す (キャッシュには本物と同じ分割表・ドライバが入っている)。控えと違えば BW_CHANGED を返し、
 *      待ち IPL は「電源を入れ直してください」と出して止まる (古い分割表のまま起動させない)。
 *
 * 書き込みや比較は接続タスク、bw_disk_read / bw_reply は USB タスクから呼ばれる。
 */
#include <stdio.h>
#include <string.h>

#include "bootwait.h"
#include "bootwait1.inc"                /* driver/bootwait1.S: 待ち IPL の前半 (HDS の LBA 2) */
#include "bootwait.inc"                 /* driver/bootwait.S: 後半 (ID 6 の BW_STAGE2) */

#define HDR_MAGIC       "X6Z BootSnap v1"
#define DATA_OFF        4096
#define DATA_SIZE       (BW_SNAP_SECTS * 512)

struct bw_hdr {
    char magic[16];
    uint32_t hash;                      /* 中身 16KB の FNV-1a */
    uint32_t size_lo, size_hi;          /* HDS の大きさ */
    uint32_t pad;
    char path[128];                     /* config の HDS0 */
};

_Static_assert(sizeof(bootwait1) <= 512, "bootwait1.S must fit in LBA 2");
_Static_assert(sizeof(bootwait) <= BW_STAGE2_SECTS * 512, "bootwait.S must fit in BW_STAGE2");

static int usable = -1;                 /* 控えが壊れていないか (-1: まだ見ていない) */
static volatile bool served;            /* この USB の接続で控えを Z に返した */
static volatile bool checked, match;    /* HDS を開いて比べた / 控えと同じだった */
static uint32_t ipl_sum;                /* BW_READY で渡す IPL の和 (待ち IPL が確かめる) */

static const struct bw_hdr *hdr(void)
{
    return (const struct bw_hdr *)bw_flash_ptr();
}

static const uint8_t *snap(void)
{
    return bw_flash_ptr() + DATA_OFF;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, int n)
{
    while (n-- > 0)
        h = (h ^ *p++) * 16777619u;
    return h;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool bw_usable(const char *path)
{
    if (usable < 0)
        usable = memcmp(hdr()->magic, HDR_MAGIC, sizeof HDR_MAGIC) == 0 &&
                 hdr()->hash == fnv(2166136261u, snap(), DATA_SIZE);
    return usable && path[0] != '\0' && strncmp(hdr()->path, path, sizeof hdr()->path) == 0;
}

int bw_disk_read(uint32_t lba, uint8_t *buf)
{
    if (lba >= BW_SNAP_SECTS)
        return -1;
    if (lba == 2 || lba == 3) {
        memset(buf, 0, 512);
        if (lba == 2)
            memcpy(buf, bootwait1, sizeof(bootwait1));
    } else {
        memcpy(buf, snap() + lba * 512, 512);
    }
    served = true;
    return 0;
}

int bw_stage2_read(uint32_t lba, uint8_t *buf)
{
    uint32_t off = (lba - BW_STAGE2) * 512;
    memset(buf, 0, 512);
    if (lba < BW_STAGE2 || lba >= BW_STAGE2 + BW_STAGE2_SECTS)
        return -1;
    if (off < sizeof(bootwait))
        memcpy(buf, bootwait + off, sizeof(bootwait) - off < 512 ? sizeof(bootwait) - off : 512);
    return 0;
}

void bw_check(const char *path, uint64_t size, bw_read_fn rd, void *ctx)
{
    static uint8_t buf[512];
    bool same = bw_usable(path) && hdr()->size_lo == (uint32_t)size && hdr()->size_hi == (uint32_t)(size >> 32);

    for (int i = 0; i < BW_SNAP_SECTS; i++) {
        if (rd(ctx, i, buf) != 0) {
            printf("bootwait: cannot read HDS LBA %d\n", i);
            match = false;
            checked = true;
            return;
        }
        if (same && memcmp(buf, snap() + i * 512, 512) != 0)
            same = false;
    }
    match = same;
    checked = true;
    if (same) {
        printf("bootwait: snapshot matches %s\n", path);
        return;
    }

    /* 控え直す。見出しを最後に書くので、途中で止まっても壊れた控えは使われない */
    printf("bootwait: writing snapshot of %s\n", path);
    bw_flash_erase(0, BW_FLASH_SIZE);
    uint32_t h = 2166136261u;
    for (int i = 0; i < BW_SNAP_SECTS; i++) {
        if (rd(ctx, i, buf) != 0)
            return;
        h = fnv(h, buf, 512);
        bw_flash_program(DATA_OFF + i * 512, buf, 512);
    }
    static uint8_t hb[256];
    struct bw_hdr *n = (struct bw_hdr *)hb;
    memset(hb, 0xff, sizeof hb);
    memset(n, 0, sizeof *n);
    memcpy(n->magic, HDR_MAGIC, sizeof HDR_MAGIC);
    n->hash = h;
    n->size_lo = (uint32_t)size;
    n->size_hi = (uint32_t)(size >> 32);
    strncpy(n->path, path, sizeof n->path - 1);
    bw_flash_program(0, hb, sizeof hb);
    usable = -1;
}

/* IPL (HDS の LBA 2-3) の 1 セクタ。控えと同じならフラッシュから (SMB を読まない) */
static int ipl_sector(int k, uint8_t *buf, bw_read_fn rd, void *ctx)
{
    if (match) {
        memcpy(buf, snap() + (2 + k) * 512, 512);
        return 0;
    }
    return rd(ctx, 2 + k, buf);
}

void bw_reply(uint32_t lba, uint8_t *buf, int hds_open, int sysstat, uint32_t ip, uint32_t uptime_ms,
              uint32_t retry_ms, bw_read_fn rd, void *ctx)
{
    uint32_t j = (lba - BW_BAND) % BW_STRIDE;
    int st;

    if (!hds_open)
        st = sysstat >= 5 ? BW_NOHDS : BW_WAIT;         /* 5 = STAT_CONFIGURED */
    else if (!checked)
        st = BW_WAIT;
    else if (served && !match)
        st = BW_CHANGED;
    else
        st = BW_READY;

    memset(buf, 0, 512);
    if (j == 0) {
        if (st == BW_READY) {
            /* 和を先に出す (セクタ 1-2 はこのあとすぐ読まれる。READY は戻らないので同じ中身) */
            uint32_t sum = 0;
            for (int k = 0; k < 2; k++) {
                if (ipl_sector(k, buf, rd, ctx) != 0) {
                    st = BW_WAIT;
                    break;
                }
                for (int i = 0; i < 512; i += 4)
                    sum += get32(buf + i);
            }
            ipl_sum = sum;
            memset(buf, 0, 512);
        }
        memcpy(buf, "X6BW", 4);
        buf[4] = st;
        buf[5] = sysstat;
        put32(buf + 8, ip);
        put32(buf + 12, uptime_ms);
        put32(buf + 16, lba);
        put32(buf + 20, st == BW_READY ? ipl_sum : 0);
        put32(buf + 24, retry_ms);
    } else if (j <= 2 && st == BW_READY) {
        if (ipl_sector(j - 1, buf, rd, ctx) != 0)
            memset(buf, 0, 512);
    }
}

const char *bw_state(const char *path)
{
    if (!checked)
        return bw_usable(path) ? "stored" : "none";
    if (served)
        return match ? "served" : "changed";
    return match ? "ok" : "updated";
}
