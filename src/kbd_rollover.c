// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - USB keyboard keys for LVGL's keypad input (kbd_rollover.h)
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#include "kbd_rollover.h"   /* kbd_keys_t */

/* One event: true when LVGL has to see a change (a press or a release) */
static bool take(kbd_keys_t *k, uint16_t code, uint32_t key, int state) {
    bool same = k->down && code == k->code;

    if (state == 0) {               /* up: only the key LVGL holds, the others were let go */
        if (same) k->down = false;
        return same;
    }
    if (key == 0 || same) {         /* no character, or the auto-repeat: LVGL repeats itself */
        return false;
    }
    if (k->down) {                  /* overlap: release now, press on the next read */
        k->queued = true;
        k->queued_code = code;
        k->queued_key = key;
        k->down = false;
        return true;
    }
    k->down = true;
    k->code = code;
    k->key = key;
    return true;
}

void kbd_keys_read(kbd_keys_t *k, kbd_event_fn next, void *arg, lv_indev_data_t *data) {
    bool        change = false;
    uint16_t    code = 0;
    uint32_t    key = 0;
    int         state;

    if (k->queued) {
        k->queued = false;
        k->down = true;
        k->code = k->queued_code;
        k->key = k->queued_key;
        change = true;
    }
    while (!change && next(arg, &code, &key, &state)) {
        change = take(k, code, key, state);
    }
    data->key = k->key;
    data->state = k->down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = change;    /* after a change more events may be waiting */
}
