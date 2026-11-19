/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The keyboard's keys with no character, as LVGL gets them (see kbd_keymap.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "kbd_keymap.h"

#include <linux/input.h>

uint32_t kbd_keymap(uint16_t code) {
    switch (code) {
        case KEY_SCROLLLOCK:    return KEYBOARD_SCRL_LOCK;
        case KEY_SYSRQ:                                     /* Print Screen on a PC keyboard */
        case KEY_PRINT:         return KEYBOARD_PRINT;
        default:                return 0;
    }
}

bool kbd_keymap_main_only(uint16_t code) {
    uint32_t key = kbd_keymap(code);

    return key == KEYBOARD_SCRL_LOCK || key == KEYBOARD_PRINT;
}

int kbd_keymap_volume(uint16_t code) {
    switch (code) {
        case KEY_VOLUMEUP:      return +1;
        case KEY_VOLUMEDOWN:    return -1;
        default:                return 0;
    }
}

int kbd_keymap_brackets(uint16_t code) {
    switch (code) {
        case KEY_RIGHTBRACE:    return +1;
        case KEY_LEFTBRACE:     return -1;
        default:                return 0;
    }
}
