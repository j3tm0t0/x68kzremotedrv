/*
 * hidmouse_cmd.c -- UDP 6869 の "mouse ..." を HID マウスレポート (相対) の列にする
 *
 *   mouse move <dx> <dy>          動かす (右・下が正)。step ずつのレポートに分け、斜めはまっすぐに
 *   mouse down [<b>] / up [<b>]   押したまま / 離す。b: left (既定) right middle (l r m でもよい)
 *   mouse click [<b>] [<n>]       n 回クリック (既定 1。2 でダブルクリック)
 *   mouse drag <dx> <dy> [<b>]    押して動かして離す
 *   mouse wheel <n>               ホイール (上が正)。1 つずつ送る
 *   mouse home                    左上の隅へ (大きく動かして画面の端で止める)。ここから move で場所を決める
 *   mouse release                 ボタンを全部離す
 *   mouse step <1-127>            1 レポートで動かす最大 (既定 8)。以後のコマンドにも残る
 *   mouse rate <ms>               動かすレポートの間隔 (既定 10)。以後のコマンドにも残る
 * Z の画面で何ドット動くかは Z のマウスの扱いによる (実機で合わせる)。
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hidmouse_cmd.h"

enum { V_MOVE = 1, V_DOWN, V_UP, V_CLICK, V_DRAG, V_WHEEL, V_HOME, V_RELEASE };

static int step = HIDMOUSE_STEP;
static int rate = HIDMOUSE_RATE_MS;
static uint8_t held;                    /* 押したままのボタン (down / up / drag で変わり、コマンドをまたいで残る) */

uint8_t hidmouse_held(void)
{
    return held;
}

static bool button(const char *w, uint8_t *b)
{
    if (w == NULL || strcasecmp(w, "left") == 0 || strcasecmp(w, "l") == 0)
        *b = HIDMOUSE_LEFT;
    else if (strcasecmp(w, "right") == 0 || strcasecmp(w, "r") == 0)
        *b = HIDMOUSE_RIGHT;
    else if (strcasecmp(w, "middle") == 0 || strcasecmp(w, "m") == 0)
        *b = HIDMOUSE_MIDDLE;
    else
        return false;
    return true;
}

static bool number(const char *w, int lo, int hi, int *v)
{
    char *e;
    if (w == NULL)
        return false;
    long n = strtol(w, &e, 10);
    if (*e != '\0' || e == w || n < lo || n > hi)
        return false;
    *v = (int)n;
    return true;
}

static void plan(struct hidmouse_cmd *c, int dx, int dy, int st)
{
    int m = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    c->dx = dx;
    c->dy = dy;
    c->step = st;
    c->steps = (m + st - 1) / st;
    c->i = 0;
}

/* line は "mouse" で始まる 1 行 */
const char *hidmouse_parse(struct hidmouse_cmd *c, char *line)
{
    char *w[5] = {0};
    int n = 0;
    for (char *p = strtok(line, " \t"); p != NULL; p = strtok(NULL, " \t"))
        if (n < 5)
            w[n++] = p;
        else
            return "err mouse usage";
    memset(c, 0, sizeof *c);
    const char *v = n > 1 ? w[1] : "";
    int a, b;
    if (strcmp(v, "move") == 0 && n == 4 && number(w[2], -32767, 32767, &a) && number(w[3], -32767, 32767, &b)) {
        c->verb = V_MOVE;
        plan(c, a, b, step);
    } else if (strcmp(v, "drag") == 0 && (n == 4 || n == 5) && number(w[2], -32767, 32767, &a) &&
               number(w[3], -32767, 32767, &b) && button(w[4], &c->btn)) {
        c->verb = V_DRAG;
        plan(c, a, b, step);
    } else if ((strcmp(v, "down") == 0 || strcmp(v, "up") == 0) && n <= 3 && button(w[2], &c->btn)) {
        c->verb = v[0] == 'd' ? V_DOWN : V_UP;
    } else if (strcmp(v, "click") == 0 && n <= 4) {
        c->verb = V_CLICK;
        c->count = 1;
        int k = 2;
        if (k < n && button(w[k], &c->btn))
            k++;
        else
            c->btn = HIDMOUSE_LEFT;
        if (k < n && !number(w[k++], 1, 10, &c->count))
            return "err mouse usage";
        if (k < n)
            return "err mouse usage";
    } else if (strcmp(v, "wheel") == 0 && n == 3 && number(w[2], -100, 100, &c->count)) {
        c->verb = V_WHEEL;
    } else if (strcmp(v, "home") == 0 && n == 2) {
        c->verb = V_HOME;
        plan(c, -HIDMOUSE_HOME, -HIDMOUSE_HOME, 127);
    } else if (strcmp(v, "release") == 0 && n == 2) {
        c->verb = V_RELEASE;
    } else if (strcmp(v, "step") == 0 && n == 3 && number(w[2], 1, 127, &a)) {
        step = a;
        return "ok";
    } else if (strcmp(v, "rate") == 0 && n == 3 && number(w[2], 1, 1000, &a)) {
        rate = a;
        return "ok";
    } else {
        return "err mouse usage: move dx dy | down/up [b] | click [b] [n] | drag dx dy [b] | wheel n | home | release | step n | rate ms";
    }
    return NULL;
}

/* 動かす 1 レポート。終わっていたら false */
static bool move_step(struct hidmouse_cmd *c, struct hidmouse_report *r)
{
    if (c->i >= c->steps)
        return false;
    /* i 番目までの累計の差 (斜めでも合計がちょうど dx, dy になる) */
    long i = c->i, s = c->steps;
    r->dx = (int8_t)(c->dx * (i + 1) / s - c->dx * i / s);
    r->dy = (int8_t)(c->dy * (i + 1) / s - c->dy * i / s);
    r->ms = rate;
    c->i++;
    return true;
}

bool hidmouse_next(struct hidmouse_cmd *c, struct hidmouse_report *r)
{
    memset(r, 0, sizeof *r);
    switch (c->verb) {
    case V_MOVE:
    case V_HOME:
        r->buttons = held;
        return move_step(c, r);
    case V_DOWN:
    case V_UP:
    case V_RELEASE:
        if (c->phase++)
            return false;
        if (c->verb == V_DOWN)
            held |= c->btn;
        else
            held &= c->verb == V_UP ? ~c->btn : 0;
        r->buttons = held;
        r->ms = HIDMOUSE_TAP_MS;
        return true;
    case V_CLICK:
        if (c->count <= 0)
            return false;
        r->ms = HIDMOUSE_TAP_MS;
        if (c->phase == 0) {
            r->buttons = held | c->btn;
            c->phase = 1;
        } else {
            r->buttons = held & ~c->btn;
            c->phase = 0;
            c->count--;
        }
        return true;
    case V_DRAG:
        if (c->phase == 0) {
            held |= c->btn;
            r->buttons = held;
            r->ms = HIDMOUSE_TAP_MS;
            c->phase = 1;
            return true;
        }
        if (c->phase == 1) {
            r->buttons = held;
            if (move_step(c, r))
                return true;
            c->phase = 2;
        }
        if (c->phase == 2) {
            held &= ~c->btn;
            r->buttons = held;
            r->ms = HIDMOUSE_TAP_MS;
            c->phase = 3;
            return true;
        }
        return false;
    case V_WHEEL:
        if (c->count == 0)
            return false;
        r->buttons = held;
        r->wheel = c->count > 0 ? 1 : -1;
        c->count -= r->wheel;
        r->ms = 30;
        return true;
    }
    return false;
}
