/*
 * bootwait.S -- 起動待ちの IPL の後半 (src/bootwait.c、BOOTWAIT_SCREEN)
 *
 * Pico が HDS をまだ開けていないとき、HDS の LBA 2 (IPL) の代わりに前半 bootwait1.S を返す。ほかの LBA 0-31 は
 * 前回控えた本物なので、Z のキャッシュには本物の分割表と SCSI ドライバが入る。これは後半で、前半が
 * SCSI ID 6 の LBA BW_STAGE2 から $2800 に読む (16KB まで。中身は変わらないのでキャッシュされて構わない)。
 *
 * 画面: 枠 (罫線)、"Pico Z Loader" のロゴ (テキスト VRAM に直接描く絵。電源の輪と Z、PICO Z / LOADER)、
 * Wi-Fi / SMB / HDS / 経過の欄、
 * Pico から Z へ ◆ が走る行、反転の案内。文字列・紋章・配置は mkbootwait.py が作る bootwait_gen.S。
 * 0.5 秒ごとに ID 6 の BW_BAND (src/bootwait_proto.h) を毎回違う番地で読み、
 *   BW_READY   -> 画面を元に戻し (パレット・プレーン 2-3・カーソル)、本物の IPL (1KB) を $2000 に置いて飛ぶ
 *                 (ROM から来たときの d4 = SCSI ID を戻す)
 *   BW_CHANGED -> 「電源を入れ直してください」と出して止まる
 *   それ以外   -> 欄を書き直す
 * S か F1 で設定画面 (hdsboot.S の F1 と同じ settingui を ID 6 から読む。画面は元に戻してから)。
 *
 * 書き直す欄は毎回 ESC[y;xH で位置を決め、決まった幅まで空白で埋める。描く前に CRTC R21 (同時アクセス) と
 * R23 (ビットマスク) に 0 を書く (設定画面から戻ると残っていて、書き直した字が前の字と重なった)。
 * Unicorn (ホスト試験) は movem を扱えないので使わない。
 */

    .text

BW_BAND     = 0x40000
BW_SLOTS    = 12288
BW_STRIDE_SHIFT = 6                     /* 64 セクタ */

V_ID        = 0x7000                    /* ROM から来たときの d4 (前半が置く) */
V_LAST      = 0x7004                    /* 最後に問い合わせた時刻 (ONTIME) */
V_UP        = 0x7008                    /* 最後に受け取った Pico の起動からの ms */
V_UP0       = 0x700c                    /* 最初に受け取った Pico の起動からの ms (経過秒の起点) */
V_SPIN      = 0x7010
V_NORESP    = 0x7012
V_PAL       = 0x7020                    /* 元のテキストパレット 16 色 */
V_HDR       = 0x7040                    /* 最後に受け取った応答の見出し (32 バイト) */
LINE        = 0x7100                    /* 表示する欄を組み立てる */
BUF         = 0x7400                    /* 応答 3 セクタ */

TVRAM_P0    = 0xe00000                  /* テキスト VRAM。ロゴは 4 プレーン、IOCS の字はプレーン 0・1 */
TVRAM_P1    = 0xe20000
TVRAM_P2    = 0xe40000
TVRAM_P3    = 0xe60000
TPAL        = 0xe82200
CRTC_R21    = 0xe8002a
CRTC_R23    = 0xe8002e

H_STATE     = 4                         /* 応答の見出し (bootwait_proto.h) */
H_SYS       = 5
H_IP        = 8
H_UPTIME    = 12
H_LBA       = 16
H_SUM       = 20
H_RETRY     = 24

entry:
    .ascii  "BW2!"                      /* 前半が確かめる */

main:
    lea.l   V_PAL.w,%a1                 /* 元のパレットを控える */
    lea.l   TPAL,%a0
    moveq.l #15,%d0
1:
    move.w  %a0@+,%a1@+
    dbra    %d0,1b
    lea.l   V_HDR.w,%a0                 /* まだ何も聞いていない: 状態 0、IP なし、再接続の予定なし */
    moveq.l #7,%d0
2:
    clr.l   %a0@+
    dbra    %d0,2b
    moveq.l #-1,%d0
    move.l  %d0,V_HDR+H_RETRY.w
    clr.l   V_UP.w
    clr.l   V_UP0.w
    clr.w   V_SPIN.w
    clr.w   V_NORESP.w
    bsr     screen_init
    moveq.l #0x7f,%d0                   /* ONTIME */
    trap    #15
    subi.l  #100,%d0
    move.l  %d0,V_LAST.w                /* 最初はすぐ問い合わせる */

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
    bpl     2f
    neg.l   %d0                         /* 逆に進む時計でも 0.5 秒ごとに (日付の変わり目はすぐ問い合わせる) */
2:
    cmpi.l  #50,%d0
    blt     loop

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
    move.l  #0xf5,%d0                   /* SCSIDRV */
    moveq.l #0x26,%d1                   /* S_READEXT */
    trap    #15
    tst.l   %d0
    bne     noresp
    movea.w #BUF,%a0
    cmpi.l  #0x58364257,%a0@            /* "X6BW" */
    bne     noresp
    cmp.l   %a0@(H_LBA),%d7             /* 読んだ番地の答えか */
    bne     noresp
    move.l  %a0@(H_UPTIME),%d0
    cmp.l   V_UP.w,%d0
    bcs     loop                        /* 前に読んだものより古い (キャッシュ): 見ない */
    move.l  %d0,V_UP.w
    tst.l   V_UP0.w
    bne     3f
    move.l  %d0,V_UP0.w
3:
    clr.w   V_NORESP.w
    lea.l   V_HDR.w,%a1                 /* 見出しを控える (読めなかったときも前の状態を出す) */
    moveq.l #7,%d0
4:
    move.l  %a0@+,%a1@+
    dbra    %d0,4b
    move.b  V_HDR+H_STATE.w,%d0
    cmpi.b  #1,%d0
    beq     ready
    cmpi.b  #2,%d0
    beq     changed
    bra     redraw

noresp:
    move.w  #1,V_NORESP.w

redraw:
    bsr     draw_status
    bsr     draw_track
    bra     loop

changed:
    bsr     draw_status
    lea.l   %pc@(m_changed),%a4
    moveq.l #C_WARN,%d6                 /* 反転 */
    bsr     draw_msg
halt:
    bra     halt

ready:                                  /* IPL の和を確かめてから画面を戻し、$2000 に写して飛ぶ */
    movea.w #BUF+512,%a1
    move.w  #255,%d1
    moveq.l #0,%d0
1:
    add.l   %a1@+,%d0
    dbra    %d1,1b
    cmp.l   V_HDR+H_SUM.w,%d0
    bne     noresp
    bsr     screen_restore
    movea.w #BUF+512,%a0
    movea.w #0x2000,%a1
    move.w  #255,%d0
2:
    move.l  %a0@+,%a1@+
    dbra    %d0,2b
    move.l  V_ID.w,%d4
    jmp     0x2000.w

loadsetting:                            /* hdsboot.S と同じ: ID 6 の設定画面を読んで飛ぶ (画面は戻してから) */
    bsr     screen_restore
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

/****************************************************************************
 * 画面
 ****************************************************************************/

/* 画面を作る: パレット、カーソルを消す、消去、紋章、枠と動かない文字 */
screen_init:
    bsr     txreset
    lea.l   %pc@(pal_set),%a0
1:
    move.w  %a0@+,%d0
    cmpi.w  #0xffff,%d0
    beq     2f
    lea.l   TPAL,%a1
    move.w  %a0@+,%a1@(%d0.w)
    bra     1b
2:
    moveq.l #0x1f,%d0                   /* B_CUROFF */
    trap    #15
    moveq.l #C_TEXT,%d6
    lea.l   %pc@(s_cls),%a1
    bsr     cprint
    bsr     clear_art
    lea.l   %pc@(art_p01),%a0           /* ロゴの行には IOCS の字を出さないので 4 プレーンとも使う (0 と 1 は同じ絵) */
    lea.l   TVRAM_P0+ART_OFF,%a1
    bsr     blit
    lea.l   %pc@(art_p01),%a0
    lea.l   TVRAM_P1+ART_OFF,%a1
    bsr     blit
    lea.l   %pc@(art_p2),%a0
    lea.l   TVRAM_P2+ART_OFF,%a1
    bsr     blit
    lea.l   %pc@(art_p3),%a0
    lea.l   TVRAM_P3+ART_OFF,%a1
    bsr     blit
    moveq.l #C_DIM,%d6
    lea.l   %pc@(s_frame),%a1
    bsr     cprint
    moveq.l #C_OK,%d6
    lea.l   %pc@(s_sub),%a1
    bsr     cprint
    moveq.l #C_TEXT,%d6
    lea.l   %pc@(s_title),%a1
    bsr     cprint
    lea.l   %pc@(s_labels),%a1
    bsr     cprint
    lea.l   %pc@(s_trk_l),%a1
    bsr     cprint
    lea.l   %pc@(s_trk_r),%a1
    bsr     cprint
    moveq.l #C_BAR,%d6
    lea.l   %pc@(s_bar),%a1
    bsr     cprint
    rts

/* 本物の IPL・設定画面に渡す前に戻す: テキスト VRAM、パレット、色、消去、カーソル */
screen_restore:
    bsr     txreset
    bsr     clear_art
    lea.l   V_PAL.w,%a0
    lea.l   TPAL,%a1
    moveq.l #15,%d0
1:
    move.w  %a0@+,%a1@+
    dbra    %d0,1b
    moveq.l #C_TEXT,%d6
    lea.l   %pc@(s_cls),%a1
    bsr     cprint
    moveq.l #0x1e,%d0                   /* B_CURON */
    trap    #15
    rts

/* テキスト VRAM 4 プレーンの見えるところ (512 ラスタ) を消す (ロゴは 4 プレーンに描くので IOCS の消去に任せない) */
clear_art:
    lea.l   TVRAM_P0,%a0
    moveq.l #3,%d1
1:
    move.w  #512*128/4-1,%d0
2:
    clr.l   %a0@+
    dbra    %d0,2b
    adda.l  #0x20000-512*128,%a0        /* 次のプレーン */
    dbra    %d1,1b
    rts

/* a0 の絵 (ART_WB バイト x ART_H ラスタ) を a1 のテキスト VRAM に (1 ラスタ = 128 バイト) */
blit:
    move.w  #ART_H-1,%d1
1:
    movea.l %a1,%a2
    move.w  #ART_WB-1,%d0
2:
    move.b  %a0@+,%a2@+
    dbra    %d0,2b
    lea.l   %a1@(128),%a1
    dbra    %d1,1b
    rts

/* 欄を書き直す: Wi-Fi・SMB・HDS・経過・IP・再接続・知らせ */
draw_status:
    moveq.l #0,%d2
    move.b  V_HDR+H_SYS.w,%d2           /* sysstatus */
    /* Wi-Fi: 0 未接続 / 1 接続中 / 2 以上 接続済み */
    lea.l   %pc@(e_wifi),%a1
    lea.l   %pc@(v_wifi0),%a4
    moveq.l #C_TEXT,%d6
    tst.b   %d2
    beq     1f
    lea.l   %pc@(v_wifi1),%a4
    cmpi.b  #1,%d2
    beq     1f
    lea.l   %pc@(v_wifi2),%a4
    moveq.l #C_OK,%d6
1:
    bsr     field
    /* SMB: 2 未満 ― / 2 接続待ち / 3 接続中 / 4 以上 接続済み */
    lea.l   %pc@(e_smb),%a1
    lea.l   %pc@(v_smb0),%a4
    moveq.l #C_DIM,%d6
    cmpi.b  #2,%d2
    bcs     2f
    lea.l   %pc@(v_smb1),%a4
    moveq.l #C_TEXT,%d6
    cmpi.b  #2,%d2
    beq     2f
    lea.l   %pc@(v_smb2),%a4
    cmpi.b  #3,%d2
    beq     2f
    lea.l   %pc@(v_smb3),%a4
    moveq.l #C_OK,%d6
2:
    bsr     field
    /* HDS: 開けた / 開けない / sysstatus 4 以上 開いています / 待っています */
    lea.l   %pc@(e_hds),%a1
    lea.l   %pc@(v_hds3),%a4
    moveq.l #C_OK,%d6
    move.b  V_HDR+H_STATE.w,%d0
    cmpi.b  #1,%d0
    beq     3f
    lea.l   %pc@(v_hds2),%a4
    moveq.l #C_WARN,%d6
    cmpi.b  #3,%d0
    beq     3f
    lea.l   %pc@(v_hds1),%a4
    moveq.l #C_TEXT,%d6
    cmpi.b  #4,%d2
    bcc     3f
    lea.l   %pc@(v_hds0),%a4
3:
    bsr     field
    /* 経過 N 秒 (Pico の時計で数える) */
    movea.w #LINE,%a3
    movea.l %a3,%a5
    move.l  V_UP.w,%d0
    sub.l   V_UP0.w,%d0
    bsr     putsec
    lea.l   %pc@(m_sec),%a4
    bsr     copystr
    moveq.l #VALUE_W,%d7
    bsr     padw
    lea.l   %pc@(e_time),%a1
    moveq.l #C_TEXT,%d6
    bsr     lprint
    /* IP */
    movea.w #LINE,%a3
    movea.l %a3,%a5
    move.l  V_HDR+H_IP.w,%d5
    beq     4f
    lea.l   %pc@(m_ip),%a4
    bsr     copystr
    moveq.l #3,%d4
5:
    rol.l   #8,%d5
    moveq.l #0,%d0
    move.b  %d5,%d0
    bsr     putdec
    move.b  #'.',%a3@+
    dbra    %d4,5b
    subq.l  #1,%a3                      /* 最後の '.' */
4:
    moveq.l #EXTRA_W,%d7
    bsr     padw
    lea.l   %pc@(e_ip),%a1
    moveq.l #C_TEXT,%d6
    bsr     lprint
    /* 次の再接続まで */
    movea.w #LINE,%a3
    movea.l %a3,%a5
    move.l  V_HDR+H_RETRY.w,%d0
    cmpi.l  #-1,%d0
    beq     6f
    lea.l   %pc@(m_trying),%a4
    tst.l   %d0
    beq     7f
    lea.l   %pc@(m_next),%a4
    bsr     copystr
    addi.l  #999,%d0
    bsr     putsec
    lea.l   %pc@(m_secp),%a4
7:
    bsr     copystr
6:
    moveq.l #EXTRA_W,%d7
    bsr     padw
    lea.l   %pc@(e_retry),%a1
    moveq.l #C_TEXT,%d6
    bsr     lprint
    /* 知らせ */
    lea.l   %pc@(m_none),%a4
    moveq.l #C_TEXT,%d6
    tst.w   V_NORESP.w
    beq     8f
    lea.l   %pc@(m_noresp),%a4
    moveq.l #C_WARN,%d6
    bra     9f
8:
    cmpi.b  #3,V_HDR+H_STATE.w
    bne     9f
    lea.l   %pc@(m_nohds),%a4
    moveq.l #C_WARN,%d6
9:
    bra     draw_msg

/* 知らせの行に a4 を d6 の色で (MSG_W 桁まで埋める) */
draw_msg:
    movea.w #LINE,%a3
    movea.l %a3,%a5
    bsr     copystr
    moveq.l #MSG_W,%d7
    bsr     padw
    lea.l   %pc@(e_msg),%a1
    bra     lprint

/* Pico から Z へ ◆ が走る: ━ x TRACK_LEN の V_SPIN 番目を ◆ に */
draw_track:
    moveq.l #0,%d0
    move.w  V_SPIN.w,%d0
    addq.w  #1,V_SPIN.w
    divu.w  #TRACK_LEN,%d0
    swap    %d0
    move.w  %d0,%d3                     /* ◆ の位置 */
    move.w  #PAL12,%d1                  /* ロゴの輪 (色 12) を 0.5 秒ごとに明るく / 元に */
    btst    #0,%d3
    beq     3f
    move.w  #PAL12_PULSE,%d1
3:
    move.w  %d1,TPAL+12*2
    lea.l   %pc@(e_track),%a1
    moveq.l #C_DIM,%d6
    bsr     cprint
    moveq.l #0,%d4
1:
    lea.l   %pc@(m_rail),%a1
    moveq.l #C_DIM,%d6
    cmp.w   %d4,%d3
    bne     2f
    lea.l   %pc@(m_packet),%a1
    moveq.l #C_OK,%d6
2:
    bsr     cprint
    addq.w  #1,%d4
    cmpi.w  #TRACK_LEN,%d4
    bne     1b
    rts

/* a1 (位置の ESC) を出してから LINE (a3 まで) を d6 の色で */
lprint:
    bsr     cprint
    clr.b   %a3@
    movea.w #LINE,%a1
    bra     cprint

/* a4 (欄の中身、幅はそろえてある) を a1 の位置に d6 の色で */
field:
    bsr     cprint
    movea.l %a4,%a1

/* a1 を d6 の色で出す */
cprint:
    bsr     txreset
    move.l  %d6,%d1
    moveq.l #0x22,%d0                   /* B_COLOR */
    trap    #15
    moveq.l #0x21,%d0                   /* B_PRINT */
    trap    #15
    rts

/* 設定画面 (IOCS の TXBOX など) が残したテキストの同時アクセスとビットマスクを戻す。残っていると
   書き直した文字が前の文字と重なって見える。clr は先に読むので move で書く (R21 は読まない) */
txreset:
    move.w  #0,CRTC_R21
    move.w  #0,CRTC_R23
    rts

/* a4 の文字列 (0 終わり) を a3 に足す */
copystr:
    move.b  %a4@+,%d1
    beq     1f
    move.b  %d1,%a3@+
    bra     copystr
1:
    rts

/* a5 から d7 バイト (= 桁) になるまで空白で埋める */
padw:
    move.l  %a3,%d1
    sub.l   %a5,%d1
    cmp.w   %d7,%d1
    bcc     1f
    move.b  #' ',%a3@+
    bra     padw
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

/* d0 (0-65535) を 10 進で a3 に足す (d0, d1 を使う) */
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

m_none:
    .byte   0
    .even

#include "bootwait_gen.S"

    .end
