/* msc_disk.c の BOOTWAIT (NOT READY で待たせる、sense の書き換え) をホストで試験する: make -C test/bootwait */
#define BOOTWAIT 1
#include <stdio.h>
#include <stdint.h>
uint32_t fake_ms;
static int next_rc;
int vd_read_block(uint32_t lba, uint8_t *buf) { return next_rc; }
int vd_write_block(uint32_t lba, uint8_t *buf) { return next_rc; }
#include "msc_disk.c"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); fails++; } } while (0)
static void sense(uint8_t *k, uint8_t *a, uint8_t *q) { uint8_t b[18] = {0x70,0,0x02,0,0,0,0,10,0,0,0,0,0x3a,0}; int n = tud_msc_request_sense_cb(0, b, 512); CHECK(n == 18, "len %d", n); *k = b[2]; *a = b[12]; *q = b[13]; }
int main(void) {
    uint8_t buf[512], k, a, q; int32_t r;
    /* generic -1 (e.g. root dir beyond the first sector) stays success */
    next_rc = -1; r = tud_msc_read10_cb(0, 0x4021, 0, buf, 512); CHECK(r == 512, "generic -1 -> %d", (int)r);
    /* not ready: 0 (re-invoke) until 5 s, then -1 and sense 02/04/01 */
    next_rc = VD_NOTREADY; fake_ms = 1000;
    r = tud_msc_read10_cb(0, 0x803fa0, 0, buf, 512); CHECK(r == 0, "first notready -> %d", (int)r);
    fake_ms = 5999; r = tud_msc_read10_cb(0, 0x803fa0, 0, buf, 512); CHECK(r == 0, "4.999 s -> %d", (int)r);
    fake_ms = 6000; r = tud_msc_read10_cb(0, 0x803fa0, 0, buf, 512); CHECK(r == -1, "5 s -> %d", (int)r);
    sense(&k, &a, &q); CHECK(k == 2 && a == 4 && q == 1, "sense %x/%x/%x", k, a, q);
    sense(&k, &a, &q); CHECK(k == 2 && a == 0x3a, "sense not cleared: %x/%x", k, a);   /* untouched afterwards */
    CHECK(msc_notready == 1, "notready count %u", msc_notready);
    /* Linux retries the same LBA: deferral starts again */
    fake_ms = 7000; r = tud_msc_read10_cb(0, 0x803fa0, 0, buf, 512); CHECK(r == 0, "retry -> %d", (int)r);
    /* becomes ready during the deferral */
    next_rc = 0; fake_ms = 8000; r = tud_msc_read10_cb(0, 0x803fa0, 0, buf, 512); CHECK(r == 512, "ready -> %d", (int)r);
    r = tud_msc_read10_cb(0, 0x803fa1, 0, buf, 512); CHECK(r == 512, "next sector -> %d", (int)r);
    /* I/O error -> -1, 03/11/00 */
    next_rc = VD_IOERR; r = tud_msc_read10_cb(0, 0x803fa2, 0, buf, 512); CHECK(r == -1, "ioerr -> %d", (int)r);
    sense(&k, &a, &q); CHECK(k == 3 && a == 0x11 && q == 0, "sense %x/%x/%x", k, a, q);
    /* write not ready: same deferral */
    next_rc = VD_NOTREADY; fake_ms = 20000; r = tud_msc_write10_cb(0, 0x803fb0, 0, buf, 512); CHECK(r == 0, "write defer -> %d", (int)r);
    fake_ms = 25001; r = tud_msc_write10_cb(0, 0x803fb0, 0, buf, 512); CHECK(r == -1, "write timeout -> %d", (int)r);
    sense(&k, &a, &q); CHECK(k == 2 && a == 4 && q == 1, "write sense %x/%x/%x", k, a, q);
    printf("%s (%u reads, %u writes counted)\n", fails ? "FAILED" : "all passed", msc_reads, msc_writes);
    return fails != 0;
}
