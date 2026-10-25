/*
 * hidkbd_cmd.c -- UDP 6869 のコマンド (picokbd と同じ) を HID キーボードレポートの列にする
 *
 *   key esc enter ...        1 つずつ押して離す
 *   combo ctrl f11 del       1 つずつ押し足し (Ctrl を先に押した状態にする)、hold ms 押したまま待って離す
 *   hold <ms>                以後の combo の押している時間 (既定 100。リセットは 600 が要る)
 *   type <文字列>             ASCII を US 配列の文字として打つ (打てない文字は飛ばす)
 * キーコードは US 配列の HID のまま送る。Z は JIS のスキャンコードとして受け取るので、記号の違いは Z 側
 * (KeyWitch の us.env) で直す (picokbd と同じ)。
 * picokbd との違い: 知らないキー名があれば何も押さずに "err key" を返す (picokbd は手前まで押していた)。
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "class/hid/hid.h"
#include "hidkbd_cmd.h"

enum { V_KEY = 1, V_COMBO, V_TYPE };

static int hold = HIDKBD_HOLD_MS;
static const uint8_t ascii[128][2] = { HID_ASCII_TO_KEYCODE };

static const struct { const char *name; uint8_t code; uint8_t mod; } names[] = {
    { "esc", HID_KEY_ESCAPE }, { "enter", HID_KEY_ENTER }, { "ret", HID_KEY_ENTER },
    { "tab", HID_KEY_TAB }, { "bs", HID_KEY_BACKSPACE }, { "space", HID_KEY_SPACE },
    { "del", HID_KEY_DELETE }, { "ins", HID_KEY_INSERT }, { "home", HID_KEY_HOME }, { "end", HID_KEY_END },
    { "pgup", HID_KEY_PAGE_UP }, { "pgdn", HID_KEY_PAGE_DOWN },
    { "up", HID_KEY_ARROW_UP }, { "down", HID_KEY_ARROW_DOWN }, { "left", HID_KEY_ARROW_LEFT }, { "right", HID_KEY_ARROW_RIGHT },
    { "f1", HID_KEY_F1 }, { "f2", HID_KEY_F2 }, { "f3", HID_KEY_F3 }, { "f4", HID_KEY_F4 },
    { "f5", HID_KEY_F5 }, { "f6", HID_KEY_F6 }, { "f7", HID_KEY_F7 }, { "f8", HID_KEY_F8 },
    { "f9", HID_KEY_F9 }, { "f10", HID_KEY_F10 }, { "f11", HID_KEY_F11 }, { "f12", HID_KEY_F12 },
    { "yen", 0x89 }, { "ro", 0x87 },   /* JIS 配列の円記号と ro (HID の Intl3 / Intl1)。US の文字では打てない \ | _ のため */
    { "ctrl", 0, KEYBOARD_MODIFIER_LEFTCTRL }, { "shift", 0, KEYBOARD_MODIFIER_LEFTSHIFT },
    { "alt", 0, KEYBOARD_MODIFIER_LEFTALT }, { "gui", 0, KEYBOARD_MODIFIER_LEFTGUI },
};

/* 1 語をキーにする。英数字 1 文字も受け付ける。戻り値: 見つかれば true */
static bool lookup(const char *w, uint8_t *code, uint8_t *mod)
{
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(w, names[i].name) == 0) {
            *code = names[i].code;
            *mod = names[i].mod;
            return true;
        }
    if (strlen(w) == 1) {
        unsigned char c = w[0];
        if (c < 128 && ascii[c][1]) {
            *code = ascii[c][1];
            *mod = ascii[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
            return true;
        }
    }
    return false;
}

int hidkbd_hold(void)
{
    return hold;
}

const char *hidkbd_parse(struct hidkbd_cmd *c, char *line)
{
    char *save, *verb = strtok_r(line, " ", &save);
    memset(c, 0, sizeof *c);
    if (verb == NULL)
        return "err empty";
    if (strcmp(verb, "type") == 0) {
        c->verb = V_TYPE;
        c->text = save ? save : "";
        return NULL;
    }
    if (strcmp(verb, "hold") == 0) {
        char *w = strtok_r(NULL, " ", &save);
        hold = w ? atoi(w) : HIDKBD_HOLD_MS;
        if (hold < 0)
            hold = 0;
        if (hold > 10000)
            hold = 10000;
        return "ok";
    }
    if (strcmp(verb, "combo") == 0)
        c->verb = V_COMBO;
    else if (strcmp(verb, "key") == 0)
        c->verb = V_KEY;
    else
        return "err verb";
    for (char *w; (w = strtok_r(NULL, " ", &save)) != NULL; ) {
        for (char *q = w; *q; q++)
            *q = tolower((unsigned char)*q);
        if (c->n >= HIDKBD_MAXW)
            return "err long";
        if (!lookup(w, &c->code[c->n], &c->mod[c->n]))
            return "err key";
        c->n++;
    }
    return NULL;
}

/* 次に送るレポート。戻り値: 終わりなら false */
bool hidkbd_next(struct hidkbd_cmd *c, struct hidkbd_report *r)
{
    memset(r, 0, sizeof *r);
    r->ms = HIDKBD_TAP_MS;
    switch (c->verb) {
    case V_TYPE:
        if (!c->up) {
            unsigned char ch;
            while ((ch = *c->text) != '\0' && (ch >= 128 || ascii[ch][1] == 0))
                c->text++;
            if (ch == '\0')
                return false;
            r->mod = ascii[ch][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
            r->keys[0] = ascii[ch][1];
            c->text++;
        }
        c->up = !c->up;
        return true;
    case V_KEY:
        if (c->i >= c->n)
            return false;
        if (!c->up) {
            r->mod = c->mod[c->i];
            r->keys[0] = c->code[c->i];
        } else {
            c->i++;
        }
        c->up = !c->up;
        return true;
    case V_COMBO:
        if (c->i > c->n)
            return false;
        if (c->i < c->n) {
            c->cmod |= c->mod[c->i];
            if (c->code[c->i] && c->nk < 6)
                c->ckeys[c->nk++] = c->code[c->i];
            r->mod = c->cmod;
            memcpy(r->keys, c->ckeys, 6);
            if (c->i == c->n - 1)
                r->ms += hold;          /* 全部押したら hold ms そのまま */
        }
        c->i++;                         /* c->i == c->n の回は全部離す */
        return true;
    }
    return false;
}
