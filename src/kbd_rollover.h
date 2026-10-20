// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - USB keyboard keys for LVGL's keypad input (kbd_rollover.c)
//
// LVGL's keypad holds one key at a time, and fast typing overlaps keys: the next one goes
// down before the last one is up. That key used to be lost. Here the key LVGL holds is let
// go first and the new one pressed on the next read; the late release of a key already let
// go is ignored. Keys are told apart by scancode, as Shift can change the character of a
// key while it is down.
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#ifndef X6100_KBD_ROLLOVER_H
#define X6100_KBD_ROLLOVER_H

#include "lvgl/lvgl.h"   /* lv_indev_data_t */

/* Next keyboard event, false when none is waiting. key: LVGL key or character, 0 for a key
 * without one (Shift, Ctrl). state: as evdev, 0 up, 1 down, 2 auto-repeat */
typedef bool (*kbd_event_fn)(void *arg, uint16_t *scancode, uint32_t *key, int *state);

typedef struct kbd_keys {
    bool        down;           /* LVGL sees key pressed */
    uint16_t    code;           /* its scancode */
    uint32_t    key;            /* last key given to LVGL, also once released */
    bool        queued;         /* a key to press on the next read */
    uint16_t    queued_code;
    uint32_t    queued_key;
} kbd_keys_t;

/* Body of the keypad's read_cb: takes events from next until one changes what LVGL sees */
void kbd_keys_read(kbd_keys_t *k, kbd_event_fn next, void *arg, lv_indev_data_t *data);

#endif /* X6100_KBD_ROLLOVER_H */
