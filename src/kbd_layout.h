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

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *label;      /* in Settings */
    const char *layout;     /* xkb's layout name */
    const char *model;      /* xkb's model: pc105 for a keyboard with the key left of Z (< > |) */
} kbd_layout_t;

/* The setting is the index into the list, so new ones go at its end */
#define KBD_LAYOUTS 15

/* The layout at index i, US for an index out of the list */
const kbd_layout_t *kbd_layout_get(int i);

/* The labels, one a line, for a dropdown: " US \n UK \n ..." */
const char *kbd_layout_options(void);

#ifdef __cplusplus
}
#endif
