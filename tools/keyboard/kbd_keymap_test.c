/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the keyboard's keys with no character (src/kbd_keymap.c; #175): F1-F10, PgUp, PgDn, Scroll Lock
 *  and Print Screen get the keys screen_main.c knows them by, by scancode, as xkb gives them none; the four
 *  without an F are the main screen's only; the letters and the others are left to xkb.
 */

#include "kbd_keymap.h"

#include <linux/input.h>
#include <stdio.h>

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

int main(void) {
    static const struct {
        uint16_t    code;
        uint32_t    key;
        bool        main_only;
        const char *name;
    } want[] = {
        {KEY_F1, KEYBOARD_F1, false, "F1"},           {KEY_F5, KEYBOARD_F5, false, "F5"},
        {KEY_F9, KEYBOARD_F9, false, "F9"},           {KEY_F10, KEYBOARD_F10, false, "F10"},
        {KEY_PAGEUP, KEYBOARD_PGUP, true, "PgUp"},    {KEY_PAGEDOWN, KEYBOARD_PGDN, true, "PgDn"},
        {KEY_SCROLLLOCK, KEYBOARD_SCRL_LOCK, true, "Scroll Lock"},
        {KEY_SYSRQ, KEYBOARD_PRINT, true, "Print Screen (SysRq)"}, {KEY_PRINT, KEYBOARD_PRINT, true, "Print"},
        {KEY_F11, 0, false, "F11 (not mapped, as before)"}, {KEY_F12, 0, false, "F12 (not mapped, as before)"},
        {KEY_L, 0, false, "L"},                       {KEY_A, 0, false, "A"},
        {KEY_ENTER, 0, false, "Enter"},               {KEY_ESC, 0, false, "Esc"},
        {KEY_CAPSLOCK, 0, false, "CapsLock"},         {KEY_LEFTSHIFT, 0, false, "Shift"},
        {KEY_HOME, 0, false, "Home"},                 {KEY_KP9, 0, false, "keypad 9"},
    };
    char line[96];

    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        snprintf(line, sizeof(line), "%s: key 0x%X, want 0x%X", want[i].name, kbd_keymap(want[i].code), want[i].key);
        check(kbd_keymap(want[i].code) == want[i].key, line);
        snprintf(line, sizeof(line), "%s: main screen only %d, want %d", want[i].name,
                 kbd_keymap_main_only(want[i].code), want[i].main_only);
        check(kbd_keymap_main_only(want[i].code) == want[i].main_only, line);
    }
    /* Every key with a value from here, and only those: F1-F10 and the four */
    int mapped = 0, main_only = 0;
    for (uint16_t code = 0; code < KEY_CNT; code++) {
        mapped += kbd_keymap(code) != 0;
        main_only += kbd_keymap_main_only(code);
    }
    snprintf(line, sizeof(line), "%d keys mapped, want 15 (F1-F10, PgUp, PgDn, Scroll Lock, SysRq, Print)", mapped);
    check(mapped == 15, line);
    snprintf(line, sizeof(line), "%d keys for the main screen only, want 5", main_only);
    check(main_only == 5, line);

    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
