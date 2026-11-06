/* hidmouse_cmd.c の試験: コマンド 1 行から出るレポートの列 (ボタン, dx, dy, ホイール, 待ち ms) と返事 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hidmouse_cmd.h"

#define T  HIDMOUSE_TAP_MS
#define R  HIDMOUSE_RATE_MS
#define L  HIDMOUSE_LEFT
#define RB HIDMOUSE_RIGHT

struct exp { uint8_t b; int dx, dy, w; uint32_t ms; };

static int fails, checks;

/* 実行してレポートを全部集める。戻り値: 個数 (返事があれば -1、*res に入れる) */
static int collect(const char *line, struct hidmouse_report *out, int max, const char **res)
{
    static struct hidmouse_cmd c;
    static char buf[512];
    snprintf(buf, sizeof buf, "%s", line);
    *res = hidmouse_parse(&c, buf);
    if (*res != NULL)
        return -1;
    int n = 0;
    struct hidmouse_report r;
    while (hidmouse_next(&c, &r)) {
        if (n < max)
            out[n] = r;
        n++;
    }
    return n;
}

static void run(const char *name, const char *line, const char *want, const struct exp *e, int n)
{
    struct hidmouse_report r[64];
    const char *res;
    int got = collect(line, r, 64, &res);
    checks++;
    if ((res == NULL) != (want == NULL) || (res && strncmp(res, want, strlen(want)) != 0)) {
        printf("FAIL %s: reply %s, want %s\n", name, res ? res : "(run)", want ? want : "(run)");
        fails++;
        return;
    }
    if (res != NULL) {
        printf("ok   %s -> %.40s\n", name, res);
        return;
    }
    if (n < 0) {                        /* 数と終わりだけ見る (to: home 33 + 間 1 + 粗 1 + 細 1) */
        int ok = got == 36 && r[33].ms == HIDMOUSE_SETTLE_MS && r[34].dy == 64 && r[35].dy == 1;
        printf("%s %s (%d reports)\n", ok ? "ok  " : "FAIL", name, got);
        fails += !ok;
        return;
    }
    int bad = got != n;
    for (int i = 0; i < got && i < n && !bad; i++)
        if (r[i].buttons != e[i].b || r[i].dx != e[i].dx || r[i].dy != e[i].dy || r[i].wheel != e[i].w || r[i].ms != e[i].ms) {
            printf("FAIL %s: step %d got b=%x %d,%d w=%d ms=%u want b=%x %d,%d w=%d ms=%u\n", name, i,
                   r[i].buttons, r[i].dx, r[i].dy, r[i].wheel, r[i].ms, e[i].b, e[i].dx, e[i].dy, e[i].w, e[i].ms);
            bad = 1;
        }
    if (bad) {
        if (got != n)
            printf("FAIL %s: %d reports, want %d\n", name, got, n);
        fails++;
    } else {
        printf("ok   %s (%d reports)\n", name, got);
    }
}

/* 大きな move: 合計がちょうど dx, dy、1 レポートは step 以内、斜めはまっすぐ */
static void sums(const char *name, const char *line, int dx, int dy, int step, int nwant)
{
    struct hidmouse_report r[2000];
    const char *res;
    int n = collect(line, r, 2000, &res);
    int sx = 0, sy = 0, bad = 0;
    checks++;
    for (int i = 0; i < n && i < 2000; i++) {
        sx += r[i].dx;
        sy += r[i].dy;
        if (abs(r[i].dx) > step || abs(r[i].dy) > step)
            bad = 1;
    }
    if (res || sx != dx || sy != dy || bad || n != nwant) {
        printf("FAIL %s: n=%d sum %d,%d (want %d %d,%d) bad=%d\n", name, n, sx, sy, nwant, dx, dy, bad);
        fails++;
    } else {
        printf("ok   %s (%d reports, sum %d,%d)\n", name, n, sx, sy);
    }
}

int main(void)
{
    run("move small", "mouse move 3 -2", NULL, (struct exp[]){ {0, 3, -2, 0, R} }, 1);
    run("move split", "mouse move 20 0", NULL, (struct exp[]){ {0, 6, 0, 0, R}, {0, 7, 0, 0, R}, {0, 7, 0, 0, R} }, 3);
    run("move zero", "mouse move 0 0", NULL, NULL, 0);
    run("click", "mouse click", NULL, (struct exp[]){ {L, 0, 0, 0, T}, {0, 0, 0, 0, T} }, 2);
    run("double right", "mouse click right 2", NULL,
        (struct exp[]){ {RB, 0, 0, 0, T}, {0, 0, 0, 0, T}, {RB, 0, 0, 0, T}, {0, 0, 0, 0, T} }, 4);
    run("click count only", "mouse click 2", NULL,
        (struct exp[]){ {L, 0, 0, 0, T}, {0, 0, 0, 0, T}, {L, 0, 0, 0, T}, {0, 0, 0, 0, T} }, 4);
    run("down", "mouse down", NULL, (struct exp[]){ {L, 0, 0, 0, T} }, 1);
    run("move held", "mouse move 0 5", NULL, (struct exp[]){ {L, 0, 5, 0, R} }, 1);
    run("down right too", "mouse down r", NULL, (struct exp[]){ {L | RB, 0, 0, 0, T} }, 1);
    run("up left", "mouse up", NULL, (struct exp[]){ {RB, 0, 0, 0, T} }, 1);
    run("release", "mouse release", NULL, (struct exp[]){ {0, 0, 0, 0, T} }, 1);
    run("drag", "mouse drag 10 -10", NULL,
        (struct exp[]){ {L, 0, 0, 0, T}, {L, 5, -5, 0, R}, {L, 5, -5, 0, R}, {0, 0, 0, 0, T} }, 4);
    run("drag middle", "mouse drag 1 0 m", NULL,
        (struct exp[]){ {HIDMOUSE_MIDDLE, 0, 0, 0, T}, {HIDMOUSE_MIDDLE, 1, 0, 0, R}, {0, 0, 0, 0, T} }, 3);
    run("wheel", "mouse wheel -2", NULL, (struct exp[]){ {0, 0, 0, -1, 30}, {0, 0, 0, -1, 30} }, 2);
    run("step", "mouse step 50", "ok", NULL, 0);
    sums("move after step 50", "mouse move 100 -30", 100, -30, 50, 2);
    run("rate", "mouse rate 20", "ok", NULL, 0);
    run("move rate 20", "mouse move 1 1", NULL, (struct exp[]){ {0, 1, 1, 0, 20} }, 1);
    run("step back", "mouse step 8", "ok", NULL, 0);
    run("rate back", "mouse rate 10", "ok", NULL, 0);
    sums("diagonal", "mouse move 1000 -333", 1000, -333, 8, 125);
    sums("home", "mouse home", -HIDMOUSE_HOME, -HIDMOUSE_HOME, 127, (HIDMOUSE_HOME + 126) / 127);
    run("by", "mouse by 32 -2", NULL, (struct exp[]){ {0, 64, 0, 0, 80}, {0, 64, 0, 0, 80},
        {0, 1, -1, 0, 50}, {0, 1, -1, 0, 50} }, 4);
    run("to", "mouse to 0 16", NULL, NULL, -1);
    run("to range", "mouse to -1 0", "err mouse usage", NULL, 0);
    run("cal", "mouse cal 32 7", "ok", NULL, 0);
    run("by cal", "mouse by -8 0", NULL, (struct exp[]){ {0, -32, 0, 0, 80}, {0, -1, 0, 0, 50} }, 2);
    run("cal back", "mouse cal 64 15", "ok", NULL, 0);
    run("bad verb", "mouse jump", "err mouse usage", NULL, 0);
    run("bare", "mouse", "err mouse usage", NULL, 0);
    run("bad num", "mouse move 1 x", "err mouse usage", NULL, 0);
    run("bad button", "mouse down x", "err mouse usage", NULL, 0);
    run("step range", "mouse step 128", "err mouse usage", NULL, 0);
    run("click extra", "mouse click l 2 3", "err mouse usage", NULL, 0);
    printf("%d/%d passed\n", checks - fails, checks);
    return fails != 0;
}
