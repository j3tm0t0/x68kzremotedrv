/* hidkbd_cmd.c の試験: コマンド 1 行から出るレポートの列 (修飾, キー, 待ち ms) と返事を、picokbd の動きと突き合わせる */
#include <stdio.h>
#include <string.h>
#include "class/hid/hid.h"
#include "hidkbd_cmd.h"

#define T  HIDKBD_TAP_MS
#define LC KEYBOARD_MODIFIER_LEFTCTRL
#define LS KEYBOARD_MODIFIER_LEFTSHIFT

struct exp { uint8_t mod; uint8_t k[6]; uint32_t ms; };

static int fails, checks;

/* line を実行し、返事 want (NULL = 実行して "ok") とレポート列 e[0..n-1] を比べる */
static void run(const char *name, const char *line, const char *want, const struct exp *e, int n)
{
    static struct hidkbd_cmd c;
    char buf[512];
    struct hidkbd_report r;
    int i = 0, bad = 0;
    checks++;
    snprintf(buf, sizeof buf, "%s", line);
    const char *res = hidkbd_parse(&c, buf);
    if ((res == NULL) != (want == NULL) || (res && strcmp(res, want) != 0)) {
        printf("FAIL %s: reply %s, want %s\n", name, res ? res : "(run)", want ? want : "(run)");
        fails++;
        return;
    }
    if (res != NULL) {
        printf("ok   %s -> %s\n", name, res);
        return;
    }
    while (hidkbd_next(&c, &r)) {
        if (i >= n || r.mod != e[i].mod || memcmp(r.keys, e[i].k, 6) != 0 || r.ms != e[i].ms) {
            printf("FAIL %s: step %d got mod=%02x keys=%02x,%02x,%02x ms=%u", name, i, r.mod, r.keys[0], r.keys[1], r.keys[2], r.ms);
            if (i < n)
                printf(" want mod=%02x keys=%02x,%02x,%02x ms=%u", e[i].mod, e[i].k[0], e[i].k[1], e[i].k[2], e[i].ms);
            printf("\n");
            bad = 1;
            break;
        }
        i++;
    }
    if (!bad && i != n) {
        printf("FAIL %s: %d steps, want %d\n", name, i, n);
        bad = 1;
    }
    if (bad)
        fails++;
    else
        printf("ok   %s (%d reports)\n", name, n);
}

#define RUN(name, line, ...) do { static const struct exp e_[] = { __VA_ARGS__ }; run(name, line, NULL, e_, sizeof e_ / sizeof e_[0]); } while (0)
#define UP  { 0, {0}, T }

int main(void)
{
    RUN("key esc", "key esc", { 0, {HID_KEY_ESCAPE}, T }, UP);
    RUN("key esc enter", "key esc enter", { 0, {HID_KEY_ESCAPE}, T }, UP, { 0, {HID_KEY_ENTER}, T }, UP);
    RUN("key upper-case names", "key ESC Ret", { 0, {HID_KEY_ESCAPE}, T }, UP, { 0, {HID_KEY_ENTER}, T }, UP);
    RUN("key single char (lowered)", "key A 1", { 0, {HID_KEY_A}, T }, UP, { 0, {HID_KEY_1}, T }, UP);
    RUN("key shifted symbol", "key !", { LS, {HID_KEY_1}, T }, UP);
    RUN("key modifier alone", "key ctrl", { LC, {0}, T }, UP);
    RUN("key f11 trailing spaces", "key  f11  ", { 0, {HID_KEY_F11}, T }, UP);

    /* 既定の hold (100ms): ctrl -> ctrl+f11 -> ctrl+f11+del を押し足し、最後だけ TAP + hold 待って全部離す */
    RUN("combo ctrl f11 del (hold 100)", "combo ctrl f11 del",
        { LC, {0}, T }, { LC, {HID_KEY_F11}, T }, { LC, {HID_KEY_F11, HID_KEY_DELETE}, T + 100 }, UP);
    run("hold 600", "hold 600", "ok", NULL, 0);
    if (hidkbd_hold() != 600) { printf("FAIL hold = %d\n", hidkbd_hold()); fails++; }
    RUN("combo after hold 600 (reset)", "combo ctrl f11 del",
        { LC, {0}, T }, { LC, {HID_KEY_F11}, T }, { LC, {HID_KEY_F11, HID_KEY_DELETE}, T + 600 }, UP);
    RUN("hold persists", "combo shift a", { LS, {0}, T }, { LS, {HID_KEY_A}, T + 600 }, UP);
    run("hold (no arg) resets", "hold", "ok", NULL, 0);
    if (hidkbd_hold() != HIDKBD_HOLD_MS) { printf("FAIL hold = %d\n", hidkbd_hold()); fails++; }
    run("hold clamps", "hold 99999", "ok", NULL, 0);
    if (hidkbd_hold() != 10000) { printf("FAIL hold = %d\n", hidkbd_hold()); fails++; }
    run("hold 100", "hold 100", "ok", NULL, 0);
    RUN("combo 7 keys keeps 6", "combo a b c d e f g",
        { 0, {4}, T }, { 0, {4,5}, T }, { 0, {4,5,6}, T }, { 0, {4,5,6,7}, T }, { 0, {4,5,6,7,8}, T },
        { 0, {4,5,6,7,8,9}, T }, { 0, {4,5,6,7,8,9}, T + 100 }, UP);
    RUN("combo with no keys", "combo", UP);

    RUN("type Ab!", "type Ab!", { LS, {HID_KEY_A}, T }, UP, { 0, {HID_KEY_B}, T }, UP, { LS, {HID_KEY_1}, T }, UP);
    RUN("type keeps spaces after the first", "type  a b",
        { 0, {HID_KEY_SPACE}, T }, UP, { 0, {HID_KEY_A}, T }, UP, { 0, {HID_KEY_SPACE}, T }, UP, { 0, {HID_KEY_B}, T }, UP);
    RUN("type skips non-ASCII", "type a\xe3\x81\x82z", { 0, {HID_KEY_A}, T }, UP, { 0, {HID_KEY_Z}, T }, UP);
    RUN("type ver + newline", "type ver\n", { 0, {HID_KEY_V}, T }, UP, { 0, {HID_KEY_E}, T }, UP, { 0, {HID_KEY_R}, T }, UP,
        { 0, {HID_KEY_ENTER}, T }, UP);
    RUN("type empty", "type");

    run("empty", "", "err empty", NULL, 0);
    run("spaces only", "   ", "err empty", NULL, 0);
    run("unknown verb", "press esc", "err verb", NULL, 0);
    run("unknown key", "key esc nosuchkey", "err key", NULL, 0);
    run("unknown key in combo", "combo ctrl f13", "err key", NULL, 0);
    {
        char big[512] = "key";
        for (int i = 0; i < 254; i++) strcat(big, " a");
        static struct hidkbd_cmd c;
        struct hidkbd_report r;
        int n = 0;
        checks++;
        if (hidkbd_parse(&c, big) != NULL) {
            printf("FAIL 254 words: not accepted\n");
            fails++;
        } else {
            while (hidkbd_next(&c, &r))
                n++;
            if (n != 508) { printf("FAIL 254 words: %d reports\n", n); fails++; }
            else printf("ok   254 words (508 reports)\n");
        }
    }

    printf("%d/%d passed\n", checks - fails, checks);
    return fails != 0;
}
