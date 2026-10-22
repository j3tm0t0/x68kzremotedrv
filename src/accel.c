/*
 * accel.c -- X68000 Z の SSH クライアント (ssh.x) の代わりに X25519 / Ed25519 を計算する (暗号アクセラレータ)
 *
 * Z の 68000 (10MHz) では ed25519 verify に 27 秒、ハンドシェイク全体で 65 秒かかる。Pico (Cortex-M0+ 125MHz) の
 * Monocypher なら数十〜数百 ms。npd.x の X6NR 交換に制御レコード (accel_proto.h) を混ぜ、Z が引数を書き、
 * Pico が計算して次の交換以降で結果を返す。
 *
 * 計算は専用の FreeRTOS タスクで行う (np_exchange は USB タスクの MSC 書き込みの中で呼ばれ、戻るまで Z の
 * SCSI 書き込みが終わらないので、そこで数百 ms 計算すると USB が止まる)。
 *   USB タスク: accel_submit() が要求を req に写して通知 -> タスクが accel_compute() -> 結果を res に置いて
 *   res_len を立てる -> 次の np_exchange で accel_result() が応答に載せる (echo_pkt / echo_len と同じ受け渡し)。
 * 同時に 1 つだけ。計算中に別の要求が来たら ACCEL_ST_BUSY を即答する。直前と同じ要求 (id と中身が一致) が
 * 来たら計算せずに同じ結果を返す (Z が交換を取りこぼして出し直したとき)。
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include "FreeRTOS.h"
#include "task.h"

#include "accel_proto.h"
#include "accel_ops.h"
#include "accel.h"

#define ACCEL_STACK     1536        /* ワード (6KB)。crypto_ed25519_check の最深経路は約 2.1KB (-fstack-usage で測った) */

static TaskHandle_t accel_th;
static uint8_t req[ACCEL_REQ_MAX];
static volatile int req_len;        /* 0 でなければ計算待ち / 計算中 */
static uint8_t res[ACCEL_RES_MAX];
static volatile int res_len;        /* 0 でなければ Z に返す応答が入っている */
static uint8_t last_req[ACCEL_REQ_MAX];
static int last_req_len;
static uint8_t last_res[ACCEL_RES_MAX];
static int last_res_len;
static uint8_t busy_res[ACCEL_RES_HDR];
static volatile int busy_len;
static uint32_t st_done, st_busy, st_dup;

static uint32_t usec_now(void) { return time_us_32(); }

static void accel_task(void *params)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int n = req_len;
        if (n == 0)
            continue;
        int m = accel_compute(req, n, last_res, usec_now);
        memcpy(last_req, req, n);
        last_req_len = n;
        last_res_len = m;
        memcpy(res, last_res, m);
        __dmb();
        res_len = m;
        req_len = 0;                /* ここで次の要求を受け付ける */
        st_done++;
        printf("accel: op %d id %u status %d %lu us\n", req[1], (unsigned)accel_get16(req + 2), last_res[4],
               (unsigned long)accel_get32(last_res + 6));
    }
}

void accel_init(void)
{
    if (accel_th != NULL)
        return;
    if (xTaskCreate(accel_task, "AccelThread", ACCEL_STACK, NULL, 1, &accel_th) != pdPASS) {
        accel_th = NULL;
        printf("accel: cannot create the task (heap?) -- accelerator disabled\n");
        return;
    }
    printf("accel: ready (X25519/Ed25519 on the Pico, Monocypher)\n");
}

/* 要求レコードの中身を受け取る (USB タスク)。計算はタスクに任せてすぐ戻る */
void accel_submit(const uint8_t *payload, int len)
{
    if (len < ACCEL_REQ_HDR || len > ACCEL_REQ_MAX || accel_th == NULL)
        return;
    if (len == last_req_len && memcmp(payload, last_req, len) == 0 && res_len == 0 && req_len == 0) {
        /* 出し直し: 同じ結果を返す */
        memcpy(res, last_res, last_res_len);
        __dmb();
        res_len = last_res_len;
        st_dup++;
        return;
    }
    if (req_len != 0 || res_len != 0) {
        /* 計算中 (または前の結果をまだ渡していない)。id を返して BUSY を即答する (Z 側は自前の計算に切り替える) */
        busy_len = accel_res_build(busy_res, payload[1], accel_get16(payload + 2), ACCEL_ST_BUSY, 0, 0, NULL, 0);
        st_busy++;
        return;
    }
    memcpy(req, payload, len);
    __dmb();
    req_len = len;
    xTaskNotifyGive(accel_th);
}

#if HID_KEYBOARD
const char *accel_state(uint32_t *done)
{
    *done = st_done;
    if (accel_th == NULL)
        return "wait";
    return req_len != 0 ? "busy" : "ready";
}
#endif

/* 応答があれば dst に写して長さを返す (USB タスク)。BUSY の即答が先 */
int accel_result(uint8_t *dst)
{
    int n = busy_len;
    if (n) {
        memcpy(dst, busy_res, n);
        busy_len = 0;
        return n;
    }
    n = res_len;
    if (n) {
        memcpy(dst, res, n);
        __dmb();
        res_len = 0;
        return n;
    }
    return 0;
}
