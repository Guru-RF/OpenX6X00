/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The keyboard's keys with no character, as LVGL gets them (kbd_keymap.c)
 *
 *  xkb gives a character or LVGL's own key (Enter, the arrows, Esc...) and nothing for the others, which
 *  were then lost: Scroll Lock and Print Screen get theirs by scancode here since #175. The F keys and PgUp
 *  and PgDn have none, so they do nothing: the keyboard's shortcuts are CapsLock's chords (kbd_hyper.h).
 *  Up to v0.3.8 F1-F10 were mapped here and did things on the main screen, in JTTY and two apps.
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
#define KEYBOARD_SCRL_LOCK  (0xFF14)
#define KEYBOARD_PRINT      (0xFF61)
#define KEYBOARD_PRINT_SCR  (0xFD1D)

/* The LVGL key of the key with this evdev scancode when xkb gives it none (Scroll Lock, Print Screen), 0 for the
 * others */
uint32_t kbd_keymap(uint16_t code);

/* The keys only the main screen uses (Scroll Lock, Print Screen): their presses go nowhere else. A text box would
 * type the keysym as stray bytes */
bool kbd_keymap_main_only(uint16_t code);

#ifdef __cplusplus
}
#endif
