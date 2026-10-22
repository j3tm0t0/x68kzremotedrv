/*
 * msclog.c -- Z からの USB MSC の命令を 1 件 1 行の UDP で送る (起動時の調査用、cmake -DMSC_LOG=ON)
 *
 * 電源投入時に Z が Pico の Wi-Fi / SMB の接続より先に起動ディスクを読んで「ディスクから起動できません」になることが
 * ある。いつ何を読み、こちらが何を返したか (待たせたか、エラーか、何が入っていたか) と、ソフトリセット (Ctrl+F11+Del)
 * のあとに Z が起動セクタを読み直すのかを見る。
 *
 * 受け手 (Raspberry Pi など、MSC_LOG_HOST:MSC_LOG_PORT) は 1 行ずつ表示するだけでよい:
 *   python3 -uc 'import socket,time;s=socket.socket(2,2);s.bind(("",6870));[print(time.strftime("%T"),s.recv(512).decode()) for _ in iter(int,1)]'
 * 行の形:
 *   <seq> <開始 ms> <所要 ms> R|W <USB の LBA> n=<セクタ数> <場所> [err] [head=<先頭 8 バイト>] [tur=<間の TEST UNIT READY 数>] [skip=<飛ばした数>]
 *   <seq> <ms> C op=<SCSI 命令> lun=<n>                   READ10/WRITE10 以外の命令 (TEST UNIT READY は数えるだけ)
 *   <seq> <ms> M <出来事>                                  sysstatus の変化、SMB 接続、HDS を開いた、vd_sync の待ち など
 *   <seq> <ms> U <mount|umount>
 * <場所> は仮想 FAT の中の名前 (bpb fat root x68z pscsi log config imgdir) か、HDS ファイルなら id<ID>:<HDS の LBA>。
 *
 * 記録は USB タスク (と接続タスク) がリングに積み、専用タスクが Wi-Fi がつながってから送る (つながる前の分も残る)。
 * 多すぎないよう、USB の mount のあとと、2 秒以上命令が途切れたあとの MSCLOG_BUDGET 件だけを送る。
 * ID 6 の netpipe と scsiremote (B:) の帯 (HDS の LBA 0x100000 以上) は記録せず数だけ数える。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/udp.h"
#include "tusb.h"

#include "main.h"
#include "vd_command.h"
#include "msclog.h"

#ifndef MSC_LOG_HOST
#define MSC_LOG_HOST    "192.168.1.108"
#endif
#ifndef MSC_LOG_PORT
#define MSC_LOG_PORT    6870
#endif
#define MSCLOG_BUDGET   300
#define MSCLOG_IDLE_MS  2000
#define RING            128

#define HDS_BASE        0x00803fa0      /* virtual_disk.c: "disk0～6.hds" の始まり */
#define HDS_SPAN        0x800000

struct rec {
    uint32_t t0, t1;
    char kind;                          /* R W C M U */
    uint8_t lun, op, err;
    uint32_t lba, n;
    uint16_t tur, skip;
    uint8_t head[8];
    char text[40];
};

static struct rec ring[RING];
static uint32_t r_head, r_tail, r_lost;  /* head: 積む側、tail: 送る側 (どちらも critical section の中で) */

/* USB タスクだけが触る: 今の READ10 / WRITE10 */
static struct rec cur;
static bool cur_open;
static int budget = MSCLOG_BUDGET;
static uint32_t last_cmd;               /* 最後の (数えるだけでない) 命令の時刻 */
static uint16_t tur_count, skip_count;

static uint32_t now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static void push(const struct rec *r)
{
    taskENTER_CRITICAL();
    if (r_head - r_tail < RING)
        ring[r_head++ % RING] = *r;
    else
        r_lost++;
    taskEXIT_CRITICAL();
}

static bool pop(struct rec *r, uint32_t *lost)
{
    bool ok = false;
    taskENTER_CRITICAL();
    if (r_head != r_tail) {
        *r = ring[r_tail++ % RING];
        ok = true;
    }
    *lost = r_lost;
    taskEXIT_CRITICAL();
    return ok;
}

/* 記録の枠を使う。2 秒以上途切れていたら枠を戻す (Z のリセットのあとを取りたい) */
static bool take_budget(uint32_t t)
{
    if (t - last_cmd >= MSCLOG_IDLE_MS)
        budget = MSCLOG_BUDGET;
    last_cmd = t;
    if (budget <= 0)
        return false;
    budget--;
    return true;
}

static void flush_cur(void)
{
    if (!cur_open)
        return;
    cur_open = false;
    cur.t1 = now_ms();
    cur.tur = tur_count;
    cur.skip = skip_count;
    if (take_budget(cur.t0)) {
        push(&cur);
        tur_count = skip_count = 0;
    }
}

void msclog_rw(char dir, uint8_t lun, uint32_t lba, int rc, const uint8_t *buf)
{
    if (lba >= HDS_BASE && (lba - HDS_BASE) / HDS_SPAN == 6 && (lba - HDS_BASE) % HDS_SPAN >= 0x100000) {
        flush_cur();
        skip_count++;                   /* netpipe / scsiremote の帯 */
        return;
    }
    if (cur_open && (cur.kind != dir || cur.lun != lun || lba != cur.lba + cur.n))
        flush_cur();                    /* 完了の通知が来なかったとき */
    if (!cur_open) {
        memset(&cur, 0, sizeof cur);
        cur.kind = dir;
        cur.lun = lun;
        cur.lba = lba;
        cur.t0 = now_ms();
        if (buf != NULL)
            memcpy(cur.head, buf, 8);
        cur_open = true;
    }
    cur.n++;
    if (rc < 0)
        cur.err = 1;
}

void tud_msc_read10_complete_cb(uint8_t lun)
{
    flush_cur();
}

void tud_msc_write10_complete_cb(uint8_t lun)
{
    flush_cur();
}

void tud_msc_scsi_complete_cb(uint8_t lun, uint8_t const scsi_cmd[16])
{
    flush_cur();
    if (scsi_cmd[0] == SCSI_CMD_TEST_UNIT_READY) {
        tur_count++;                    /* Linux は数秒ごとに出すので数えるだけ */
        return;
    }
    uint32_t t = now_ms();
    if (!take_budget(t))
        return;
    struct rec r = { .t0 = t, .t1 = t, .kind = 'C', .lun = lun, .op = scsi_cmd[0], .tur = tur_count };
    tur_count = 0;
    push(&r);
}

void msclog_usb(const char *what)
{
    flush_cur();
    budget = MSCLOG_BUDGET;
    struct rec r = { .t0 = now_ms(), .kind = 'U' };
    snprintf(r.text, sizeof r.text, "%s", what);
    push(&r);
}

void msclog_mark(const char *fmt, ...)
{
    struct rec r = { .t0 = now_ms(), .kind = 'M' };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.text, sizeof r.text, fmt, ap);
    va_end(ap);
    push(&r);
}

/* USB の LBA を仮想 FAT の中の場所にする (virtual_disk.c の割り当て) */
static void where(char *s, int size, uint32_t lba)
{
    if (lba >= HDS_BASE)
        snprintf(s, size, "id%lu:0x%lx", (unsigned long)((lba - HDS_BASE) / HDS_SPAN), (unsigned long)((lba - HDS_BASE) % HDS_SPAN));
    else if (lba < 0x20)
        snprintf(s, size, "bpb");
    else if (lba < 0x4020)
        snprintf(s, size, "fat");
    else if (lba < 0x4060)
        snprintf(s, size, "root");
    else if (lba < 0x40a0)
        snprintf(s, size, "x68z");
    else if (lba < 0x40e0)
        snprintf(s, size, "pscsi");
    else if (lba < 0x4120)
        snprintf(s, size, "log");
    else if (lba < 0x4160)
        snprintf(s, size, "config");
    else if (lba < 0x41a0)
        snprintf(s, size, "imgdir");
    else
        snprintf(s, size, "?");
}

static int format(char *s, int size, uint32_t seq, const struct rec *r, uint32_t lost)
{
    int n = snprintf(s, size, "%lu %lu ", (unsigned long)seq, (unsigned long)r->t0);
    switch (r->kind) {
    case 'R':
    case 'W': {
        char w[24];
        where(w, sizeof w, r->lba);
        n += snprintf(s + n, size - n, "%lu %c 0x%lx n=%lu %s%s", (unsigned long)(r->t1 - r->t0), r->kind,
                      (unsigned long)r->lba, (unsigned long)r->n, w, r->err ? " err" : "");
        if (r->kind == 'R') {
            n += snprintf(s + n, size - n, " head=");
            for (int i = 0; i < 8; i++)
                n += snprintf(s + n, size - n, "%02x", r->head[i]);
        }
        break;
    }
    case 'C':
        n += snprintf(s + n, size - n, "C op=0x%02x lun=%u", r->op, r->lun);
        break;
    default:
        n += snprintf(s + n, size - n, "%c %s", r->kind, r->text);
        break;
    }
    if (r->tur)
        n += snprintf(s + n, size - n, " tur=%u", r->tur);
    if (r->skip)
        n += snprintf(s + n, size - n, " skip=%u", r->skip);
    if (lost)
        n += snprintf(s + n, size - n, " lost=%lu", (unsigned long)lost);
    return n < size ? n : size - 1;
}

static void msclog_task(void *params)
{
    struct udp_pcb *pcb = NULL;
    ip_addr_t dst;
    uint32_t seq = 0, up_since = 0, prev_lost = 0;
    int last_stat = -1;

    ipaddr_aton(MSC_LOG_HOST, &dst);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (sysstatus != last_stat) {
            msclog_mark("sysstatus %d", sysstatus);
            last_stat = sysstatus;
        }
        if (sysstatus < STAT_WIFI_CONNECTED) {
            up_since = 0;
            continue;
        }
        if (up_since == 0)
            up_since = now_ms();
        if (now_ms() - up_since < 500)  /* つながった直後は ARP が引けるまで少し待つ */
            continue;
        if (pcb == NULL) {
            cyw43_arch_lwip_begin();
            pcb = udp_new();
            cyw43_arch_lwip_end();
            if (pcb == NULL)
                continue;
        }
        struct rec r;
        uint32_t lost;
        for (int i = 0; i < 8 && pop(&r, &lost); i++) {
            char line[160];
            int len = format(line, sizeof line, seq++, &r, lost != prev_lost ? lost : 0);   /* 増えたときだけ */
            prev_lost = lost;
            cyw43_arch_lwip_begin();
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
            if (p != NULL) {
                memcpy(p->payload, line, len);
                udp_sendto(pcb, p, &dst, MSC_LOG_PORT);
                pbuf_free(p);
            }
            cyw43_arch_lwip_end();
        }
    }
}

void msclog_init(void)
{
    xTaskCreate(msclog_task, "MscLogThread", 768, NULL, 1, NULL);
}
