#ifndef _HIDKBD_CMD_H_
#define _HIDKBD_CMD_H_
#include <stdint.h>
#include <stdbool.h>

/* picokbd (x68k の harness/picokbd) と同じ 1 行のコマンドを、送る HID キーボードレポートの列にする。
 * USB にも lwIP にも触らない (ホストでも試験できるように)。使い方:
 *   r = hidkbd_parse(&c, line);    NULL なら実行する。NULL 以外はそのまま返事にする ("ok" は hold、"err ...")
 *   while (hidkbd_next(&c, &rep))  rep を送り、rep.ms だけ待つ
 * line は終わるまで書き換えないこと (type は line の中を指す) */

#define HIDKBD_TAP_MS   40              /* 押してから離すまで / 次のキーまで */
#define HIDKBD_HOLD_MS  100             /* combo の押している時間の既定 (hold で変え、以後のコマンドにも残る) */
#define HIDKBD_MAXW     256             /* key / combo の語の数 (1 行 511 文字なら超えない) */

struct hidkbd_report {
    uint8_t mod;
    uint8_t keys[6];
    uint32_t ms;                        /* このレポートを送ってから次を送るまで */
};

struct hidkbd_cmd {
    int verb;
    int n;                              /* key / combo: 語の数 */
    uint8_t code[HIDKBD_MAXW], mod[HIDKBD_MAXW];
    const char *text;                   /* type: 次の文字 */
    int i;                              /* key / combo: 次の語 */
    bool up;                            /* key / type: 次は離す */
    uint8_t cmod, ckeys[6];             /* combo: 押し足したもの */
    int nk;
};

const char *hidkbd_parse(struct hidkbd_cmd *c, char *line);
bool hidkbd_next(struct hidkbd_cmd *c, struct hidkbd_report *r);
int hidkbd_hold(void);

#endif
