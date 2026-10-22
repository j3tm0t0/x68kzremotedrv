/*
 * bootwait_pico.c -- bootwait.c の控えをフラッシュに書く部分 (Pico)。ホスト試験では test/bootwait が差し替える
 *
 * flash_safe_execute (pico_flash) の中で消して書く。このファームの FreeRTOS は 1 コア (コア 1 は動かさない) なので、
 * 実際には割り込みを止めるだけになる (config_write と同じ)。消すのは 1 回に 4KB (約 45ms) ずつ
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"

#include "bootwait.h"

struct op {
    bool erase;
    uint32_t off, len;
    const uint8_t *data;
};

static void do_op(void *p)
{
    const struct op *o = p;
    if (o->erase)
        flash_range_erase(BW_FLASH_OFFSET + o->off, o->len);
    else
        flash_range_program(BW_FLASH_OFFSET + o->off, o->data, o->len);
}

static void run(struct op *o)
{
    int rc = flash_safe_execute(do_op, o, 1000);
    if (rc != PICO_OK)
        printf("bootwait: flash %s 0x%lx failed (%d)\n", o->erase ? "erase" : "program", (unsigned long)o->off, rc);
}

void bw_flash_erase(uint32_t off, uint32_t len)
{
    for (uint32_t a = 0; a < len; a += FLASH_SECTOR_SIZE) {
        struct op o = { .erase = true, .off = off + a, .len = FLASH_SECTOR_SIZE };
        run(&o);
    }
}

void bw_flash_program(uint32_t off, const uint8_t *data, uint32_t len)
{
    struct op o = { .erase = false, .off = off, .len = len, .data = data };
    run(&o);
}

const uint8_t *bw_flash_ptr(void)
{
    return (const uint8_t *)(XIP_BASE + BW_FLASH_OFFSET);
}
