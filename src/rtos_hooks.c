/*
 * rtos_hooks.c -- FreeRTOS の malloc の錠、configASSERT、スタックあふれ
 *
 * malloc の錠: pico-sdk の malloc は pico の mutex (PICO_USE_MALLOC_MUTEX、pico_multicore を入れると既定で 1) で守られ、
 * FreeRTOS の heap_3 (pvPortMalloc) は vTaskSuspendAll のあとで malloc を呼ぶ。ほかのタスクが malloc の途中で
 * 切り替えられて mutex を持っていると、pvPortMalloc / vPortFree (xTaskCreate、キューやセマフォの作成、タスクの削除) は
 * スケジューラを止めたまま mutex を待つ。pico の mutex の待ちは port のイベントグループ (xEventGroupWaitBits) で、
 * スケジューラが止まっていると待てない: 同じタスクを待ちのリストに入れ直し続けてリストが壊れ、
 * vTaskPlaceOnUnorderedEventList で止まっていた (sdcopy: SdCopy タスクの libsmb2 と USB タスクの libsmb2 が同時に
 * malloc する。configASSERT が -DNDEBUG で消えていたので event_groups.c の検査も働かなかった)。
 * CMake で PICO_USE_MALLOC_MUTEX=0 にし、newlib の malloc の錠をスケジューラの停止にする。止めている間は待たないので
 * 入れ子 (heap_3 の中の malloc) でも割り込みの外ならどこからでも呼べる。
 *
 * 止まったとき (configASSERT、スタックあふれ): 割り込みを止めて UART に直接書き (printf は pico の mutex を使う)、
 * watchdog の scratch[0..3] に控えて再起動する。次の起動で "previous run stopped" として出す。
 * HardFault は lib/FatFs_SD の crash.c が isr_hardfault を持っている (RAM に控えて bkpt)。その控えも起動時に出す。
 */
#include <stdio.h>
#include <string.h>
#include <reent.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#if SD_CARD
#include "crash.h"
#endif

void __malloc_lock(struct _reent *r)
{
    (void)r;
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        vTaskSuspendAll();
}

void __malloc_unlock(struct _reent *r)
{
    (void)r;
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        xTaskResumeAll();
}

#define FATAL_MAGIC 0x46a7a100u         /* scratch[0] の上 24 ビット。下 8 ビットが what */

static const char *const what_name[] = { "assert", "stack overflow" };

static void raw_puts(const char *s)
{
#ifdef uart_default
    uart_puts(uart_default, s);
    uart_tx_wait_blocking(uart_default);
#endif
}

/* what: 0 = configASSERT (file:line)、1 = スタックあふれ (file にタスク名) */
void rtos_fatal(int what, const char *file, int line)
{
    static char buf[160];
    save_and_disable_interrupts();
    const char *task = "";
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        TaskHandle_t t = xTaskGetCurrentTaskHandle();
        if (t != NULL)
            task = pcTaskGetName(t);
    }
    if (what == 1 && file != NULL)
        task = file;                    /* スタックあふれはフックが渡すタスク名 */
    snprintf(buf, sizeof buf, "\n*** STOP: %s %s:%d task %s, rebooting\n",
             what_name[what], what == 0 && file ? file : "", line, task);
    raw_puts(buf);

    char name[8] = {0};
    strncpy(name, task, sizeof name);
    watchdog_hw->scratch[0] = FATAL_MAGIC | (uint32_t)what;
    watchdog_hw->scratch[1] = (uint32_t)line;
    memcpy((void *)&watchdog_hw->scratch[2], name, sizeof name);
    watchdog_reboot(0, 0, 10);
    while (1)
        __wfi();
}

/* main の初めに: 前の起動が rtos_fatal (か crash.c の HardFault) で止まっていたら出す */
void rtos_fatal_report(void)
{
#if SD_CARD
    crash_handler_init();
    const crash_info_t *ci = crash_handler_get_info();
    if (ci != NULL) {
        char buf[128];
        int next = 0;
        do {
            next = dump_crash_info(ci, next, buf, sizeof buf);
            printf("previous run: %s\n", buf);
        } while (next);
    }
#endif
    uint32_t s0 = watchdog_hw->scratch[0];
    if ((s0 & ~0xffu) != FATAL_MAGIC || (s0 & 0xff) >= count_of(what_name))
        return;
    char name[9] = {0};
    memcpy(name, (const void *)&watchdog_hw->scratch[2], 8);
    for (int i = 0; i < 8; i++)
        if (name[i] != '\0' && (name[i] < ' ' || name[i] > '~'))
            name[i] = '?';
    printf("previous run stopped: %s line %lu task %s\n", what_name[s0 & 0xff],
           (unsigned long)watchdog_hw->scratch[1], name);
    watchdog_hw->scratch[0] = 0;
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    rtos_fatal(1, pcTaskName, 0);
}
