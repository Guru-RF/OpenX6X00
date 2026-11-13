/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The USB or Bluetooth keyboard's layout (#236): Settings > Keys > KEYBOARD
 *
 *  xkb turns each key into its character by a layout. Up to v0.3.9 that was always the US one, so a German
 *  keyboard typed Z for Y and had / where its - is. The layouts here are xkb's own, which both radios carry
 *  (/usr/share/X11/xkb). Only the characters change: the CapsLock shortcuts and the paddle keys go by the key's
 *  place (its scancode), so they stay where a US keyboard has them.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct xkb_state;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *label;      /* in Settings */
    const char *layout;     /* xkb's layout name */
    const char *model;      /* xkb's model: pc105 for a keyboard with the key left of Z (< > |) */
    bool        caps_digits;    /* Caps Lock types the number row's digits (kbd_layout_digits_row) */
} kbd_layout_t;

/* The setting is the index into the list, so new ones go at its end */
#define KBD_LAYOUTS 15

/* The layout at index i, US for an index out of the list */
const kbd_layout_t *kbd_layout_get(int i);

/* The labels, one a line, for a dropdown: " US \n UK \n ..." */
const char *kbd_layout_options(void);

/* #254: on a French or Belgian keyboard the number row types & é " ' ( and the others, and its digits with Shift.
 * With Caps Lock on, people who type on these keyboards expect the digits, as the keyboards give them elsewhere, so a
 * callsign like F4ABC types without Shift. xkb only uppercases letters with Caps Lock (É for é) unless asked with its
 * caps:digits_row option, which the radios' xkb data is too old to carry: so it is done here, the same way. The
 * character the number-row key `code` (an evdev code) types with Caps Lock on, from xkb's state: Shift turned round
 * for that key alone, so Shift with Caps Lock types the row's own characters again (é, not É). 0 for any other key,
 * with AltGr held, or with no state: what xkb gives stays. Packed as lv_drivers' xkb.c packs a character */
uint32_t kbd_layout_digits_row(struct xkb_state *st, int code);

#ifdef __cplusplus
}
#endif
