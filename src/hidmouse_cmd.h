#ifndef _HIDMOUSE_CMD_H_
#define _HIDMOUSE_CMD_H_
#include <stdint.h>
#include <stdbool.h>

/* UDP 6869 の "mouse ..." の 1 行を、送る HID マウスレポート (相対) の列にする。hidkbd_cmd と同じく USB にも lwIP にも
 * 触らない (ホストで試験する: test/hidmouse)。使い方:
 *   r = hidmouse_parse(&c, line);   NULL なら実行する。NULL 以外はそのまま返事にする ("ok" は設定だけ、"err ...")
 *   while (hidmouse_next(&c, &rep)) rep を送り、rep.ms だけ待つ */

#define HIDMOUSE_TAP_MS     40          /* ボタンを押してから離すまで / 離してから次まで */
#define HIDMOUSE_STEP       8           /* 1 レポートで動かす最大 (mouse step で変え、以後も残る) */
#define HIDMOUSE_RATE_MS    10          /* 動かすレポートの間隔 (mouse rate で変える。bInterval 10ms に合わせる) */
#define HIDMOUSE_HOME       4096        /* mouse home: 左上へこれだけ動かす (画面の端で止まる) */

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
};

const char *hidmouse_parse(struct hidmouse_cmd *c, char *line);
bool hidmouse_next(struct hidmouse_cmd *c, struct hidmouse_report *r);
uint8_t hidmouse_held(void);            /* 今押したままのボタン */

#endif
