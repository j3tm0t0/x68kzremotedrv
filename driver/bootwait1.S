/*
 * bootwait1.S -- 起動待ちの IPL の前半 (src/bootwait.c、BOOTWAIT_SCREEN)
 *
 * Pico が HDS をまだ開けていないとき、HDS の LBA 2 (IPL) の代わりにこれを返す。ROM から来たときの d4 (SCSI ID) を
 * 控え、後半 (bootwait.S) を SCSI ID 6 の LBA BW_STAGE2 から $2800 に読んで飛ぶ。後半を ID 6 に置くのは、
 * HDS の LBA 0-31 は 16KB まとめて Z にキャッシュされ、IPL に使えるのが LBA 2-3 の 1KB だけだから。
 */

    .text

BW_STAGE2   = 0x80                      /* src/bootwait_proto.h */
V_ID        = 0x7000

entry:
    bra.w   start
    .space  4*15

start:
    move.l  %d4,V_ID.w
    move.l  #BW_STAGE2,%d2
    moveq.l #32,%d3                     /* 16KB */
    moveq.l #6,%d4                      /* SCSI ID 6 */
    moveq.l #1,%d5
    movea.w #0x2800,%a1
    move.l  #0xf5,%d0                   /* SCSIDRV */
    moveq.l #0x26,%d1                   /* S_READEXT */
    trap    #15
    tst.l   %d0
    bne     error
    cmpi.l  #0x42573221,0x2800.w        /* "BW2!" */
    bne     error
    jmp     0x2804.w

error:
    lea.l   %pc@(msg),%a1
    moveq.l #0x21,%d0                   /* B_PRINT */
    trap    #15
1:
    bra     1b

msg:
    .byte   0x1b
    .ascii  "[14;5Hbootwait: no reply from the Pico (SCSI ID 6)"
    .byte   0

    .end
