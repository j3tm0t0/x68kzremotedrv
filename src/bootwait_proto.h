#ifndef _BOOTWAIT_PROTO_H_
#define _BOOTWAIT_PROTO_H_

/* 起動待ちの IPL (driver/bootwait.S) と Pico (bootwait.c) の取り決め。数値はビッグエンディアン
 *
 * 待ち IPL は SCSI ID 6 の BW_BAND〜BW_BAND_END のどこか (毎回違う番地) から 3 セクタ読む:
 *   セクタ 0: +0 "X6BW" +4 状態 (BW_*) +5 sysstatus +6 0 +8 Pico の IP (4) +12 Pico の起動からの ms (4) +16 読んだ LBA (4)
 *            +20 IPL の和 (BW_READY のとき) +24 次の再接続までの ms (0: 試している最中、0xffffffff: 予定なし)
 *   待ち IPL は経過秒を +12 の差で数える (Z の IOCS _ONTIME ではなく Pico の時計)
 *   セクタ 1-2: 状態が BW_READY なら本物の IPL (HDS の LBA 2-3、1KB)
 * Z の Linux は読んだページをキャッシュするので、同じ番地は二度と読まない (時刻から決め、BW_STRIDE ずつ離す。
 * Linux は 1 回に 16KB 先読みするので、続きの番地にすると先読みした古い答えを読んでしまう)。
 */
#define BW_BAND         0x40000         /* ID 6 の LBA。settingui (0x100-0x1ff) と netpipe (0x100000-) の間 */
#define BW_BAND_END     0x100000
#define BW_STRIDE       64              /* セクタ (32KB) */
#define BW_SLOTS        ((BW_BAND_END - BW_BAND) / BW_STRIDE)  /* 12288 */
#define BW_REPLY_SECTS  3

#define BW_WAIT         0               /* まだ (sysstatus を見て表示) */
#define BW_READY        1               /* セクタ 1-2 の IPL を $2000 に置いて飛ぶ */
#define BW_CHANGED      2               /* HDS の先頭が控えと違う: 電源を入れ直してもらう */
#define BW_NOHDS        3               /* SMB にはつながったが HDS を開けない */

#define BW_STAGE2       0x80            /* ID 6 の LBA: 待ち IPL の後半 (driver/bootwait.S、16KB まで。中身は変わらない) */
#define BW_STAGE2_SECTS 32

#define BW_SNAP_SECTS   32              /* 控える HDS の先頭 (LBA 0-31、16KB: 署名・IPL・分割表・SCSI ドライバ) */

#endif
