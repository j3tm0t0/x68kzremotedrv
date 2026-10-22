/* bootwait.c のホスト試験用: フラッシュの代わり (消すと 0xff、書くと AND)。中身を出し入れして「再起動」を真似る */
#include <string.h>
#include "bootwait.h"

static uint8_t flash[BW_FLASH_SIZE];
static int erases, programs;

void bw_flash_erase(uint32_t off, uint32_t len)
{
    memset(flash + off, 0xff, len);
    erases++;
}

void bw_flash_program(uint32_t off, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        flash[off + i] &= data[i];
    programs++;
}

const uint8_t *bw_flash_ptr(void)
{
    return flash;
}

void fake_flash_init(void) { memset(flash, 0xff, sizeof flash); erases = programs = 0; }
void fake_flash_get(uint8_t *dst) { memcpy(dst, flash, sizeof flash); }
void fake_flash_set(const uint8_t *src) { memcpy(flash, src, sizeof flash); }
int fake_flash_writes(void) { return erases * 1000 + programs; }
