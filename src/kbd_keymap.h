/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The keyboard's keys with no character, as LVGL gets them (kbd_keymap.c)
 *
 *  xkb gives a character or LVGL's own key (Enter, the arrows, Esc...) and nothing for the others, which
 *  were then lost: F1-F10 by scancode from the start, PgUp, PgDn, Scroll Lock and Print Screen since #175.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keys of the USB keyboard with no character, as LVGL gets them (the X11 keysyms) */
#define KEYBOARD_F1         (0xFFBE)
#define KEYBOARD_F2         (0xFFBF)
#define KEYBOARD_F3         (0xFFC0)
#define KEYBOARD_F4         (0xFFC1)
#define KEYBOARD_F5         (0xFFC2)
#define KEYBOARD_F6         (0xFFC3)
#define KEYBOARD_F7         (0xFFC4)
#define KEYBOARD_F8         (0xFFC5)
#define KEYBOARD_F9         (0xFFC6)
#define KEYBOARD_F10        (0xFFC7)
#define KEYBOARD_F11        (0xFFC8)
#define KEYBOARD_F12        (0xFFC9)

#define KEYBOARD_SCRL_LOCK  (0xFF14)
#define KEYBOARD_PRINT      (0xFF61)
#define KEYBOARD_PRINT_SCR  (0xFD1D)

#define KEYBOARD_PGUP       (0xFF55)
#define KEYBOARD_PGDN       (0xFF56)

/* The LVGL key of the key with this evdev scancode when xkb gives it none (F1-F10, PgUp, PgDn, Scroll Lock,
 * Print Screen), 0 for the others */
uint32_t kbd_keymap(uint16_t code);

/* The keys only the main screen uses (PgUp, PgDn, Scroll Lock, Print Screen): their presses go nowhere else. A text
 * box would type the keysym as a character, PgUp's 0xFF55 as a U */
bool kbd_keymap_main_only(uint16_t code);

#ifdef __cplusplus
}
#endif
