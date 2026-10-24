/*
 * tsp_lwip/lwipopts.h -- TSP (tis.x -p のソケット窓口、src/tsp_svc.c) を入れるときの lwIP の設定
 *
 * libsmb2 の picow/lwipopts.h をそのまま読み、ソケット窓口に足りないものだけ大きくする (CMake の TSP=ON で
 * このディレクトリを include の先頭に置く。libsmb2 自身のビルドは元の lwipopts.h のまま)。
 * 大きさは x68k リポジトリの net/tistack/README.md「Pico の RAM」の Pico W 版:
 *   MEM_SIZE 4000 -> 8192 (tcp_write の写し: TSP_TXPOOL 4KB + 見出し・制御パケット。RAM が足りず 8KB にはできなかった)
 *   TCP pcb 5 -> 16、TCP segment 32 -> 40、UDP pcb -> 10、SO_REUSE (rcmd.x の listen し直し)
 * TCP_WND / TCP_SND_BUF (8 * MSS) と PBUF_POOL (24) は元のまま
 * mDNS: ".local" の名前を引く (src/tsp_mdns.c、UDP の口 1 個と時計 4 個) と、<ホスト名>.local に答える (lwIP の mDNS、
 *   IGMP と netif の状態の通知が要る。UDP の口 1 個と時計を数個)
 */
#include "../../libsmb2/include/picow/lwipopts.h"

#undef MEM_SIZE
#ifdef TSP_MEM_SIZE
#define MEM_SIZE                TSP_MEM_SIZE    /* Pico 2 W */
#else
#define MEM_SIZE                8192
#endif
#undef MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG        40
#undef MEMP_NUM_TCP_PCB
#define MEMP_NUM_TCP_PCB        16
#undef MEMP_NUM_UDP_PCB
#define MEMP_NUM_UDP_PCB        12
#undef SO_REUSE
#define SO_REUSE                1

#undef LWIP_IGMP
#define LWIP_IGMP               1
#undef LWIP_NETIF_EXT_STATUS_CALLBACK
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1
#define LWIP_MDNS_RESPONDER     1
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define MDNS_MAX_SERVICES       1
/* 時計: lwIP が数える分 + mDNS の答える側 (調べる・名乗る・各種の遅延で 8) + tsp_mdns.c の 4 */
#undef MEMP_NUM_SYS_TIMEOUT
#define MEMP_NUM_SYS_TIMEOUT    (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8 + 4)
