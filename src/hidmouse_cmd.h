#ifndef _HIDMOUSE_CMD_H_
#define _HIDMOUSE_CMD_H_
#include <stdint.h>
#include <stdbool.h>

/* UDP 6869 の "mouse ..." の 1 行を、送る HID マウスレポート (相対) の列にする。hidkbd_cmd と同じく USB にも lwIP にも
 * 触らない (ホストで試験する: test/hidmouse)。使い方:
 *   r = hidmouse_parse(&c, line);   NULL なら実行する。NULL 以外はそのまま返事にする ("ok" は設定だけ、"err ...")
 *   while (hidmouse_next(&c, &rep)) rep を送り、rep.ms だけ待つ */

#define HIDMOUSE_TAP_MS     100         /* ボタンを押してから離すまで / 離してから次まで (mouse tap で変える)。Z は 50ms 刻みほどでしか
                                           ボタンを見ず、40ms ではダブルクリックの 2 回目が落ちた */
#define HIDMOUSE_STEP       8           /* 1 レポートで動かす最大 (mouse step で変え、以後も残る) */
#define HIDMOUSE_RATE_MS    10          /* 動かすレポートの間隔 (mouse rate で変える。bInterval 10ms に合わせる) */
#define HIDMOUSE_HOME       4096        /* mouse home: 左上へこれだけ動かす (画面の端で止まる) */

/* mouse by / to (画面のドットで指定) の換算。Z (IOCS の _MS_CURGT) で測った値 (2026-11-06):
 *   1 レポートの量 d が 1〜8 なら 1 ドット、32 なら 7、64 なら 15 (80ms おき)。間隔が 40ms より短いとレポートが落ち、
 *   30〜40ms で大きい d を続けると加速が掛かる。そこで粗く COARSE を COARSE_MS おきに送り、残りを 1 で FINE_MS おきに送る。
 *   mouse cal <d> <ドット> で粗い方を変えられる */
#define HIDMOUSE_COARSE     64
#define HIDMOUSE_COARSE_DOTS 15
#define HIDMOUSE_COARSE_MS  80
#define HIDMOUSE_FINE_MS    50
#define HIDMOUSE_SETTLE_MS  300         /* home のあと、by / to の前に置く間 */

#define HIDMOUSE_LEFT       0x01
#define HIDMOUSE_RIGHT      0x02
#define HIDMOUSE_MIDDLE     0x04

struct hidmouse_report {
    uint8_t buttons;
    int8_t dx, dy, wheel;
    uint32_t ms;                        /* このレポートを送ってから次を送るまで */
};

struct hidmouse_cmd {
    int verb;
    int phase;
    int dx, dy;                         /* move / drag / home: 全体の量 */
    int steps, i;                       /* 何レポートに分けるか、次は何番目 */
    int step;                           /* 1 レポートの最大 (home は 127) */
    uint8_t btn;                        /* click / drag / down / up のボタン */
    int count;                          /* click: 残りの回数、wheel: 残りの量 (符号付き) */
    int cx, cy, fx, fy;                 /* by / to: 残りの粗いレポートと 1 のレポートの数 (符号付き) */
};

const char *hidmouse_parse(struct hidmouse_cmd *c, char *line);
bool hidmouse_next(struct hidmouse_cmd *c, struct hidmouse_report *r);
uint8_t hidmouse_held(void);            /* 今押したままのボタン */

#endif
