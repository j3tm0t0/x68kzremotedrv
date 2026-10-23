/*
 * tsp_plat.h -- tsp_svc.c (Pico のソケット窓口) がファームに求めるもの
 *
 * tsp_svc.c は lwIP の raw API (tcp_* / udp_* / dns_* / pbuf) だけを使う。ファームの違い (FreeRTOS + cyw43 の
 * threadsafe_background か、ポーリングだけか) はここで吸収する。ホストの試験 (tests/picosim.c) は
 * TSP_PLAT_HOST を定義してこのまま使う。
 *
 *   TSP_LOCK() / TSP_UNLOCK(): lwIP を触る間の排他。tsp_exchange() はこれで lwIP の呼び出しを囲む。
 *     pico-sdk の cyw43_arch では cyw43_arch_lwip_begin() / end()。lwIP のコールバック (受信・接続・DNS) は
 *     この外 (lwIP の文脈) で呼ばれ、tsp_svc の表を書き換えるので、交換もこの中で表を読む
 *   TSP_LOG(...): printf 相当 (Pico のログ)。無ければ空
 */
#ifndef TSP_PLAT_H
#define TSP_PLAT_H

#if defined(TSP_PLAT_HOST)
#include <stdio.h>
#define TSP_LOCK()      do { } while (0)
#define TSP_UNLOCK()    do { } while (0)
extern int tsp_log_on;
#define TSP_LOG(...)    do { if (tsp_log_on) printf(__VA_ARGS__); } while (0)
#elif defined(PICO_CYW43_ARCH_THREADSAFE_BACKGROUND) || defined(PICO_CYW43_ARCH_FREERTOS)
#include "pico/cyw43_arch.h"
#include <stdio.h>
#define TSP_LOCK()      cyw43_arch_lwip_begin()
#define TSP_UNLOCK()    cyw43_arch_lwip_end()
#define TSP_LOG(...)    printf(__VA_ARGS__)
#else
/* NO_SYS でポーリング (cyw43_arch_poll と同じ文脈から tsp_exchange を呼ぶ) */
#include <stdio.h>
#define TSP_LOCK()      do { } while (0)
#define TSP_UNLOCK()    do { } while (0)
#define TSP_LOG(...)    printf(__VA_ARGS__)
#endif

#endif
