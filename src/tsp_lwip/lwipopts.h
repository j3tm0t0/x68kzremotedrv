/*
 * tsp_lwip/lwipopts.h -- TSP (tis.x -p のソケット窓口、src/tsp_svc.c) を入れるときの lwIP の設定
 *
 * libsmb2 の picow/lwipopts.h をそのまま読み、ソケット窓口に足りないものだけ大きくする (CMake の TSP=ON で
 * このディレクトリを include の先頭に置く。libsmb2 自身のビルドは元の lwipopts.h のまま)。
 * 大きさは x68k リポジトリの net/tistack/README.md「Pico の RAM」の Pico W 版:
 *   MEM_SIZE 4000 -> 8192 (tcp_write の写し: TSP_TXPOOL 4KB + 見出し・制御パケット。RAM が足りず 8KB にはできなかった)
 *   TCP pcb 5 -> 16、TCP segment 32 -> 40、UDP pcb -> 10、SO_REUSE (rcmd.x の listen し直し)
 * TCP_WND / TCP_SND_BUF (8 * MSS) と PBUF_POOL (24) は元のまま
 */
#include "../../libsmb2/include/picow/lwipopts.h"

#undef MEM_SIZE
#define MEM_SIZE                8192
#undef MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG        40
#undef MEMP_NUM_TCP_PCB
#define MEMP_NUM_TCP_PCB        16
#undef MEMP_NUM_UDP_PCB
#define MEMP_NUM_UDP_PCB        10
#undef SO_REUSE
#define SO_REUSE                1
