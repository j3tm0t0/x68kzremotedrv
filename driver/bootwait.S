/*
 * bootwait.S -- 起動待ちの IPL (src/bootwait.c、BOOTWAIT_SCREEN)
 *
 * Pico が HDS をまだ開けていないとき、HDS の LBA 2-3 (IPL) の代わりにこれを返す。ほかの LBA 0-31 は
 * 前回控えた本物なので、Z のキャッシュには本物の分割表と SCSI ドライバが入る。
 * これは後半 (SCSI ID 6 の LBA BW_STAGE2 から $2800 に読まれる)。前半 bootwait1.S が HDS の LBA 2 に入る。
 * 画面に Pico の状態を出しながら、SCSI ID 6 の BW_BAND (src/bootwait_proto.h) を毎回違う番地で読み、
 *   BW_READY   -> 返ってきた本物の IPL (1KB) を $2000 に置いて飛ぶ (ROM から来たときの d4 = SCSI ID を戻す)
 *   BW_CHANGED -> 「電源を入れ直してください」と出して止まる
 *   それ以外   -> 状態を出して 0.5 秒後にもう一度
 * S (か F1) を押すと設定画面 (hdsboot.S の F1 と同じ settingui を ID 6 から読む)。
 * Unicorn (ホスト試験) は movem を扱えないので使わない。
 */

    .text

BW_BAND     = 0x40000
BW_SLOTS    = 12288
BW_STRIDE_SHIFT = 6                     /* 64 セクタ */

V_ID        = 0x3800                    /* ROM から来たときの d4 (前半が置く) */
V_T0        = 0x3804                    /* 始めた時刻 (1/100 秒) */
V_LAST      = 0x3808                    /* 最後に問い合わせた時刻 */
V_UP        = 0x380c                    /* 最後に受け取った Pico の起動からの ms */
V_SPIN      = 0x3810
V_UP0       = 0x3814                    /* 最初に受け取った Pico の起動からの ms (経過秒の起点) */
LINE        = 0x3900                    /* 表示する行を組み立てる */
ANIM_LEN    = 24
BUF         = 0x4000                    /* 応答 3 セクタ */

entry:
    .ascii  "BW2!"                      /* 前半が確かめる */
main:
    moveq.l #0x7f,%d0                   /* ONTIME */
    trap    #15
    move.l  %d0,V_T0.w
    subi.l  #100,%d0
    move.l  %d0,V_LAST.w                /* 最初はすぐ問い合わせる */
    clr.l   V_UP.w
    clr.l   V_UP0.w
    clr.w   V_SPIN.w
    lea.l   %pc@(m_screen),%a1
    moveq.l #0x21,%d0
    trap    #15

loop:
    moveq.l #0x04,%d0                   /* B_BITSNS */
    moveq.l #0x0c,%d1
    trap    #15
    andi.b  #0x08,%d0                   /* F1 */
    bne     loadsetting
    moveq.l #0x01,%d0                   /* B_KEYSNS */
    trap    #15
    tst.l   %d0
    beq     1f
    moveq.l #0x00,%d0                   /* B_KEYINP (読み捨てる) */
    trap    #15
    andi.b  #0xdf,%d0                   /* 大文字に */
    cmpi.b  #'S',%d0
    beq     loadsetting
1:

    moveq.l #0x7f,%d0                   /* ONTIME */
    trap    #15
    move.l  %d0,%d6
    sub.l   V_LAST.w,%d0
    bpl     1f
    neg.l   %d0                         /* 逆に進む時計でも 0.5 秒ごとに (日付の変わり目はすぐ問い合わせる) */
1:
    cmpi.l  #50,%d0
    blt     loop

poll:
    move.l  %d6,V_LAST.w
    move.l  %d6,%d0                     /* 番地 = BW_BAND + ((時刻 >> 5) mod BW_SLOTS) * 64 */
    lsr.l   #5,%d0
    divu.w  #BW_SLOTS,%d0
    clr.w   %d0
    swap    %d0
    lsl.l   #BW_STRIDE_SHIFT,%d0
    addi.l  #BW_BAND,%d0
    move.l  %d0,%d2
    move.l  %d0,%d7
    moveq.l #3,%d3
    moveq.l #6,%d4                      /* SCSI ID 6 */
    moveq.l #1,%d5
    movea.w #BUF,%a1
    move.l  #0xf5,%d0
    moveq.l #0x26,%d1
    trap    #15
    tst.l   %d0
    bne     noresp
    movea.w #BUF,%a0
    cmpi.l  #0x58364257,%a0@            /* "X6BW" */
    bne     noresp
    cmp.l   %a0@(16),%d7                /* 読んだ番地の答えか */
    bne     noresp
    move.l  %a0@(12),%d0
    cmp.l   V_UP.w,%d0
    bcs     loop                        /* 前に読んだものより古い (キャッシュ): 見ない */
    move.l  %d0,V_UP.w
    tst.l   V_UP0.w
    bne     7f
    move.l  %d0,V_UP0.w
7:
    move.b  %a0@(4),%d0
    cmpi.b  #1,%d0
    beq     ready
    cmpi.b  #2,%d0
    beq     changed
    lea.l   %pc@(m_nohds),%a2
    cmpi.b  #3,%d0
    beq     show
    moveq.l #0,%d0                      /* 待ち: sysstatus で言い分ける */
    move.b  %a0@(5),%d0
    cmpi.b  #4,%d0
    bls     1f
    moveq.l #4,%d0
1:
    add.w   %d0,%d0
    lea.l   %pc@(t_stat),%a2
    adda.w  %a2@(%d0.w),%a2
    bra     show

noresp:
    clr.l   BUF+8.w                     /* IP と再接続の予定は出さない */
    moveq.l #-1,%d0
    move.l  %d0,BUF+24.w
    lea.l   %pc@(m_noresp),%a2

show:                                   /* a2 の文言 + IP + 経過秒 + くるくる */
    movea.w #LINE,%a3
    lea.l   %pc@(e_row2),%a4
    bsr     copystr
    movea.l %a2,%a4
    bsr     copystr
    move.l  BUF+8.w,%d6
    beq     2f
    lea.l   %pc@(m_ip),%a4
    bsr     copystr
    moveq.l #3,%d5
1:
    rol.l   #8,%d6
    moveq.l #0,%d0
    move.b  %d6,%d0
    bsr     putdec
    move.b  #'.',%a3@+
    dbra    %d5,1b
    subq.l  #1,%a3                      /* 最後の '.' */
2:
    lea.l   %pc@(m_elapsed),%a4         /* 経過秒は Pico の時計で数える (Z の ONTIME は使わない) */
    bsr     copystr
    move.l  V_UP.w,%d0
    sub.l   V_UP0.w,%d0
    bsr     putsec
    lea.l   %pc@(m_sec),%a4
    bsr     copystr
    movea.w #BUF,%a0
    move.l  %a0@(24),%d0                /* 次の再接続までの ms (0: 試している最中、-1: 予定なし) */
    cmpi.l  #-1,%d0
    beq     8f
    lea.l   %pc@(m_trying),%a4
    tst.l   %d0
    beq     9f
    lea.l   %pc@(m_next),%a4
    bsr     copystr
    addi.l  #999,%d0
    bsr     putsec
    lea.l   %pc@(m_secp),%a4
9:
    bsr     copystr
8:
    move.b  #' ',%a3@+
    move.w  V_SPIN.w,%d0
    addq.w  #1,V_SPIN.w
    andi.w  #3,%d0
    lea.l   %pc@(m_spin),%a4
    move.b  %a4@(%d0.w),%a3@+
    lea.l   %pc@(m_pad),%a4
    bsr     copystr
    bsr     print
                                        /* [Pico W] .....o........ [X68000 Z] */
    movea.w #LINE,%a3
    lea.l   %pc@(m_anim1),%a4
    bsr     copystr
    moveq.l #0,%d0
    move.w  V_SPIN.w,%d0
    divu.w  #ANIM_LEN,%d0
    swap    %d0                         /* 光る位置 p */
    neg.w   %d0
    addi.w  #ANIM_LEN-1,%d0             /* dbra は数え下げなので ANIM_LEN-1-p で比べる */
    moveq.l #ANIM_LEN-1,%d1
5:
    moveq.l #'.',%d2
    cmp.w   %d0,%d1
    bne     6f
    moveq.l #'o',%d2
6:
    move.b  %d2,%a3@+
    dbra    %d1,5b
    lea.l   %pc@(m_anim2),%a4
    bsr     copystr
    bsr     print
    bra     loop

changed:
    movea.w #LINE,%a3
    lea.l   %pc@(e_row2),%a4
    bsr     copystr
    lea.l   %pc@(m_changed),%a4
    bsr     copystr
    lea.l   %pc@(m_pad),%a4
    bsr     copystr
    bsr     print
    bra     halt

ready:                                  /* IPL の和を確かめてから $2000 に写して飛ぶ (ここは $2800 から上) */
    movea.w #BUF+512,%a1
    move.w  #255,%d1
    moveq.l #0,%d0
1:
    add.l   %a1@+,%d0
    dbra    %d1,1b
    cmp.l   %a0@(20),%d0
    bne     noresp
    lea.l   %pc@(e_clear),%a1
    moveq.l #0x21,%d0
    trap    #15
    movea.w #BUF+512,%a0
    movea.w #0x2000,%a1
    move.w  #255,%d0
2:
    move.l  %a0@+,%a1@+
    dbra    %d0,2b
    move.l  V_ID.w,%d4
    jmp     0x2000.w

halt:
    bra     halt

/* a4 の文字列 (0 終わり) を a3 に足す */
copystr:
    move.b  %a4@+,%d0
    beq     1f
    move.b  %d0,%a3@+
    bra     copystr
1:
    rts

/* d0 (ms) を秒にして a3 に足す */
putsec:
    cmpi.l  #65535000,%d0
    bcs     1f
    move.l  #65535000,%d0
1:
    divu.w  #1000,%d0
    andi.l  #0xffff,%d0
    bra     putdec

/* d0 (0-65535) を 10 進で a3 に足す */
putdec:
    moveq.l #0,%d1
1:
    divu.w  #10,%d0
    swap    %d0
    addi.w  #'0',%d0
    move.w  %d0,-(%sp)
    addq.w  #1,%d1
    clr.w   %d0
    swap    %d0
    tst.w   %d0
    bne     1b
2:
    move.w  (%sp)+,%d0
    move.b  %d0,%a3@+
    subq.w  #1,%d1
    bne     2b
    rts

print:
    clr.b   %a3@
    movea.w #LINE,%a1
    moveq.l #0x21,%d0
    trap    #15
    rts

loadsetting:                            /* hdsboot.S と同じ: ID 6 の設定メニューを読んで飛ぶ */
    lea.l   %pc@(e_clear),%a1
    moveq.l #0x21,%d0
    trap    #15
    move.l  #(0x20000/512),%d2
    moveq.l #6,%d4
    moveq.l #1,%d3
    moveq.l #1,%d5
    movea.w #0x67c0,%a1
    move.l  #0xf5,%d0
    moveq.l #0x26,%d1
    trap    #15
    cmpi.w  #0x4855,0x67c0.w
    bne     halt
    move.l  0x67cc.w,%d3
    add.l   0x67d0.w,%d3
    add.l   #(0x40+511),%d3
    lsr.l   #8,%d3
    lsr.l   #1,%d3
    movea.w #0x67c0,%a1
    move.l  #0xf5,%d0
    moveq.l #0x26,%d1
    trap    #15
    movea.l 0x67c4.w,%a0
    adda.l  0x67cc.w,%a0
    adda.l  0x67d0.w,%a0
    move.l  0x67d4.w,%d0
    addq.l  #3,%d0
    lsr.l   #2,%d0
    bra     3f
2:
    clr.l   %a0@+
3:
    dbra    %d0,2b
    movea.l 0x67c8.w,%a0
    jmp     %a0@

t_stat:                                 /* sysstatus 0-4 */
    .word   m_wifi0-t_stat, m_wifi1-t_stat, m_smb0-t_stat, m_smb1-t_stat, m_hds-t_stat

m_screen:
    .byte   0x1b
    .ascii  "[2J"
    .byte   0x1b
    .ascii  "[2;13H+---------------------------------------------------------------------+"
    .byte   0x1b
    .ascii  "[3;13H|  #     #    ####    #####    #####    #####    #####       #######  |"
    .byte   0x1b
    .ascii  "[4;13H|   #   #    #       #     #  #    ##  #    ##  #    ##           #   |"
    .byte   0x1b
    .ascii  "[5;13H|    # #    #        #     #  #   # #  #   # #  #   # #          #    |"
    .byte   0x1b
    .ascii  "[6;13H|     #     ######    #####   #  #  #  #  #  #  #  #  #         #     |"
    .byte   0x1b
    .ascii  "[7;13H|    # #    #     #  #     #  # #   #  # #   #  # #   #        #      |"
    .byte   0x1b
    .ascii  "[8;13H|   #   #   #     #  #     #  ##    #  ##    #  ##    #       #       |"
    .byte   0x1b
    .ascii  "[9;13H|  #     #   #####    #####    #####    #####    #####       #######  |"
    .byte   0x1b
    .ascii  "[10;13H+---------------------------------------------------------------------+"
    .byte   0x1b
    .ascii  "[11;27H-- Raspberry Pi Pico W / x68kzremotedrv --"
    .byte   0x1b
    .ascii  "[13;5H"
    /* Pico: SMB 接続待ち… (Wi-Fi/SMB を確認してください) */
    .byte   0x50,0x69,0x63,0x6f,0x3a,0x20,0x53,0x4d,0x42,0x20,0x90,0xda,0x91,0xb1,0x91,0xd2,0x82,0xbf,0x81,0x63,0x20,0x28,0x57,0x69,0x2d,0x46,0x69,0x2f,0x53,0x4d,0x42,0x20,0x82,0xf0,0x8a,0x6d,0x94,0x46,0x82,0xb5,0x82,0xc4,0x82,0xad,0x82,0xbe,0x82,0xb3,0x82,0xa2,0x29
    .byte   0x1b
    .ascii  "[19;5H"
    /* [S] 設定画面 (Wi-Fi / SMB / HDS)   ([F1] でも入れます) */
    .byte   0x5b,0x53,0x5d,0x20,0x90,0xdd,0x92,0xe8,0x89,0xe6,0x96,0xca,0x20,0x28,0x57,0x69,0x2d,0x46,0x69,0x20,0x2f,0x20,0x53,0x4d,0x42,0x20,0x2f,0x20,0x48,0x44,0x53,0x29,0x20,0x20,0x20,0x28,0x5b,0x46,0x31,0x5d,0x20,0x82,0xc5,0x82,0xe0,0x93,0xfc,0x82,0xea,0x82,0xdc,0x82,0xb7,0x29
    .byte   0x1b
    .ascii  "[20;5H"
    /* ※ 設定で HDS を変えたら、Z の電源を入れ直してください */
    .byte   0x81,0xa6,0x20,0x90,0xdd,0x92,0xe8,0x82,0xc5,0x20,0x48,0x44,0x53,0x20,0x82,0xf0,0x95,0xcf,0x82,0xa6,0x82,0xbd,0x82,0xe7,0x81,0x41,0x5a,0x20,0x82,0xcc,0x93,0x64,0x8c,0xb9,0x82,0xf0,0x93,0xfc,0x82,0xea,0x92,0xbc,0x82,0xb5,0x82,0xc4,0x82,0xad,0x82,0xbe,0x82,0xb3,0x82,0xa2
    .byte   0
e_row2:
    .byte   0x1b
    .ascii  "[15;5H"
    .byte   0
m_anim1:
    .byte   0x1b
    .ascii  "[17;5H[Pico W] "
    .byte   0
m_anim2:
    .ascii  " [X68000 Z]"
    .byte   0
e_clear:
    .byte   0x1b
    .ascii  "[2J"
    .byte   0
m_wifi0:    /* Wi-Fi 未接続 */
    .byte   0x57,0x69,0x2d,0x46,0x69,0x20,0x96,0xa2,0x90,0xda,0x91,0xb1,0
m_wifi1:    /* Wi-Fi 接続中 */
    .byte   0x57,0x69,0x2d,0x46,0x69,0x20,0x90,0xda,0x91,0xb1,0x92,0x86,0
m_smb0:     /* SMB 接続待ち */
    .byte   0x53,0x4d,0x42,0x20,0x90,0xda,0x91,0xb1,0x91,0xd2,0x82,0xbf,0
m_smb1:     /* SMB 接続中 */
    .byte   0x53,0x4d,0x42,0x20,0x90,0xda,0x91,0xb1,0x92,0x86,0
m_hds:      /* HDS を開いています */
    .byte   0x48,0x44,0x53,0x20,0x82,0xf0,0x8a,0x4a,0x82,0xa2,0x82,0xc4,0x82,0xa2,0x82,0xdc,0x82,0xb7,0
m_nohds:    /* HDS を開けません (config.txt の HDS を確認) */
    .byte   0x48,0x44,0x53,0x20,0x82,0xf0,0x8a,0x4a,0x82,0xaf,0x82,0xdc,0x82,0xb9,0x82,0xf1,0x20,0x28,0x63,0x6f,0x6e,0x66,0x69,0x67,0x2e,0x74,0x78,0x74,0x20,0x82,0xcc,0x20,0x48,0x44,0x53,0x20,0x82,0xf0,0x8a,0x6d,0x94,0x46,0x29,0
m_noresp:   /* Pico の応答がありません */
    .byte   0x50,0x69,0x63,0x6f,0x20,0x82,0xcc,0x89,0x9e,0x93,0x9a,0x82,0xaa,0x82,0xa0,0x82,0xe8,0x82,0xdc,0x82,0xb9,0x82,0xf1,0
m_changed:  /* HDS が変わりました。Z の電源を入れ直してください */
    .byte   0x48,0x44,0x53,0x20,0x82,0xaa,0x95,0xcf,0x82,0xed,0x82,0xe8,0x82,0xdc,0x82,0xb5,0x82,0xbd,0x81,0x42,0x5a,0x20,0x82,0xcc,0x93,0x64,0x8c,0xb9,0x82,0xf0,0x93,0xfc,0x82,0xea,0x92,0xbc,0x82,0xb5,0x82,0xc4,0x82,0xad,0x82,0xbe,0x82,0xb3,0x82,0xa2,0
m_ip:
    .ascii  "  IP "
    .byte   0
m_sec:      /* 秒 */
    .byte   0x20,0x95,0x62,0
m_elapsed:  /* 経過 */
    .byte   0x20,0x20,0x8c,0x6f,0x89,0xdf,0x20,0
m_trying:   /*   (接続を試しています) */
    .byte   0x20,0x20,0x28,0x90,0xda,0x91,0xb1,0x82,0xf0,0x8e,0x8e,0x82,0xb5,0x82,0xc4,0x82,0xa2,0x82,0xdc,0x82,0xb7,0x29,0
m_next:     /*   (次の再接続まで */
    .byte   0x20,0x20,0x28,0x8e,0x9f,0x82,0xcc,0x8d,0xc4,0x90,0xda,0x91,0xb1,0x82,0xdc,0x82,0xc5,0x20,0
m_secp:     /*  秒) */
    .byte   0x20,0x95,0x62,0x29,0
m_spin:
    .ascii  ".oOo"                      /* \ は X68k では円記号なので使わない */
m_pad:
    .ascii  "                              "
    .byte   0
    .even

    .end
