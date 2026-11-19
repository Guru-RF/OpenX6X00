/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the keyboard's keys with no character (src/kbd_keymap.c; #175): Scroll Lock and Print Screen get
 *  the keys screen_main.c knows them by, by scancode, as xkb gives them none, and are the main screen's only. The F
 *  keys and PgUp and PgDn get nothing (mapped up to v0.3.8, they are CapsLock's chords now), the letters and the
 *  others are left to xkb. Volume Up and Down are the volume keys (kbd_keymap_volume): +1 and -1, the only two, and
 *  get no LVGL key.
 */

#include "kbd_keymap.h"
#include "cfg/kbd_devices.h"

#include <string.h>

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
        {KEY_SCROLLLOCK, KEYBOARD_SCRL_LOCK, true, "Scroll Lock"},
        {KEY_SYSRQ, KEYBOARD_PRINT, true, "Print Screen (SysRq)"}, {KEY_PRINT, KEYBOARD_PRINT, true, "Print"},
        {KEY_F1, 0, false, "F1 (no longer mapped)"},  {KEY_F4, 0, false, "F4 (no longer mapped)"},
        {KEY_F5, 0, false, "F5 (no longer mapped)"},  {KEY_F8, 0, false, "F8 (no longer mapped)"},
        {KEY_F9, 0, false, "F9 (no longer mapped)"},  {KEY_F10, 0, false, "F10 (no longer mapped)"},
        {KEY_F11, 0, false, "F11 (never mapped)"},    {KEY_F12, 0, false, "F12 (never mapped)"},
        {KEY_PAGEUP, 0, false, "PgUp (no longer mapped)"}, {KEY_PAGEDOWN, 0, false, "PgDn (no longer mapped)"},
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
    /* Every key with a value from here, and only those: Scroll Lock and Print Screen's two */
    int mapped = 0, main_only = 0;
    for (uint16_t code = 0; code < KEY_CNT; code++) {
        mapped += kbd_keymap(code) != 0;
        main_only += kbd_keymap_main_only(code);
    }
    snprintf(line, sizeof(line), "%d keys mapped, want 3 (Scroll Lock, SysRq, Print)", mapped);
    check(mapped == 3, line);
    snprintf(line, sizeof(line), "%d keys for the main screen only, want 3", main_only);
    check(main_only == 3, line);

    /* The volume keys: Up +1, Down -1, no other key; none of them an LVGL key */
    check(kbd_keymap_volume(KEY_VOLUMEUP) == +1, "Volume Up: +1");
    check(kbd_keymap_volume(KEY_VOLUMEDOWN) == -1, "Volume Down: -1");
    check(kbd_keymap(KEY_VOLUMEUP) == 0 && kbd_keymap(KEY_VOLUMEDOWN) == 0, "the volume keys: no LVGL key");
    check(kbd_keymap_volume(KEY_MUTE) == 0, "Mute: not a volume step");
    check(kbd_keymap_volume(KEY_LEFTBRACE) == 0 && kbd_keymap_volume(KEY_RIGHTBRACE) == 0, "[ and ]: not volume keys");
    int volume = 0;
    for (uint16_t code = 0; code < KEY_CNT; code++) {
        volume += kbd_keymap_volume(code) != 0;
    }
    snprintf(line, sizeof(line), "%d volume keys, want 2", volume);
    check(volume == 2, line);

    /* [ and ]: ] +1, [ -1, no other key (a device set to Volume, kbd_devices.h) */
    check(kbd_keymap_brackets(KEY_RIGHTBRACE) == +1 && kbd_keymap_brackets(KEY_LEFTBRACE) == -1, "] +1, [ -1");
    int brackets = 0;
    for (uint16_t code = 0; code < KEY_CNT; code++) {
        brackets += kbd_keymap_brackets(code) != 0;
    }
    snprintf(line, sizeof(line), "%d bracket keys, want 2", brackets);
    check(brackets == 2, line);

    /* The devices set to Volume (kbd_devices.c): ids, the list cleaned, set and unset */
    char id[KBD_DEV_ID_LEN], out[KBD_DEV_LIST_LEN];
    kbd_dev_id(id, BUS_BLUETOOTH, 0x514c, 0x8850);
    check(strcmp(id, "0005:514c:8850") == 0, "a Bluetooth knob's id: 0005:514c:8850");
    check(kbd_dev_listed("0003:046d:c52b 0005:514c:8850", id), "listed: found");
    check(kbd_dev_listed("0005:514C:8850", id), "listed: in capitals too");
    check(!kbd_dev_listed("0005:514c:88501", id) && !kbd_dev_listed("", id) && !kbd_dev_listed(NULL, id),
          "listed: not a longer id, not in an empty list");
    kbd_dev_list_clean("  0005:514C:8850 junk 0003:046d:c52b 0005:514c:8850 12:34:56 ", out, sizeof(out));
    check(strcmp(out, "0003:046d:c52b 0005:514c:8850") == 0, "clean: well-formed, lower case, each once, last place");
    kbd_dev_list_set("0003:046d:c52b", id, true, out, sizeof(out));
    check(strcmp(out, "0003:046d:c52b 0005:514c:8850") == 0, "set: added at the end");
    kbd_dev_list_set(out, id, true, out, sizeof(out));
    check(strcmp(out, "0003:046d:c52b 0005:514c:8850") == 0, "set again: still once");
    kbd_dev_list_set("0005:514c:8850 0003:046d:c52b", id, false, out, sizeof(out));
    check(strcmp(out, "0003:046d:c52b") == 0, "unset: taken out");
    kbd_dev_list_set("", id, false, out, sizeof(out));
    check(strcmp(out, "") == 0, "unset from nothing: nothing");
    char many[KBD_DEV_LIST_LEN * 2] = "";
    for (int i = 0; i < KBD_DEV_MAX + 3; i++) {
        kbd_dev_id(id, BUS_USB, 0x1000 + i, 0x0001);
        kbd_dev_list_set(many, id, true, out, sizeof(out));
        snprintf(many, sizeof(many), "%s", out);
    }
    kbd_dev_id(id, BUS_USB, 0x1000, 0x0001);
    check(!kbd_dev_listed(many, id), "past KBD_DEV_MAX: the oldest dropped");
    kbd_dev_id(id, BUS_USB, 0x1000 + KBD_DEV_MAX + 2, 0x0001);
    check(kbd_dev_listed(many, id) && strlen(many) == KBD_DEV_MAX * KBD_DEV_ID_LEN - 1, "past KBD_DEV_MAX: the newest kept, 16 of them");

    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
