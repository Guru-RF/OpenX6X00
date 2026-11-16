/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a press of VOL or the MFK, short or held (#282)
 *
 *  With Settings > Display > INTERFACE > Knob press: next control on, a short press of the knob takes the next control
 *  on its list and a held one does what any press did before (choose or change; on the cluster column, tune to the
 *  spot). The press is watched from its start: let go before KNOB_PRESS_HOLD_MS it is short, still down then it is
 *  held. A press with no knob down behind it (a keyboard's Enter or ESC, Field Ops' MFK click that comes at the
 *  release) is a short one at the first look. Header only, the time given by the caller (lv_tick_get() on the radio),
 *  so a host test (tools/knob_press) can check it.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KNOB_PRESS_HOLD_MS  400     /* as LVGL's long press of the MFK (LV_INDEV_DEF_LONG_PRESS_TIME) */

typedef enum {
    KNOB_PRESS_WAIT = 0,    /* still down, not long enough yet; or no press watched */
    KNOB_PRESS_SHORT,       /* let go in time */
    KNOB_PRESS_HELD,        /* down for KNOB_PRESS_HOLD_MS */
} knob_press_t;

typedef struct {
    uint32_t since;         /* ms, when it was pressed */
    bool     watching;
} knob_press_watch_t;

static inline void knob_press_begin(knob_press_watch_t *w, uint32_t now) {
    w->since    = now;
    w->watching = true;
}

/* At a look, whether the knob is down now: what the press turned out to be, once; WAIT meanwhile and after */
static inline knob_press_t knob_press_look(knob_press_watch_t *w, uint32_t now, bool down) {
    if (!w->watching) {
        return KNOB_PRESS_WAIT;
    }
    if (!down) {
        w->watching = false;
        return KNOB_PRESS_SHORT;
    }
    if ((uint32_t)(now - w->since) >= KNOB_PRESS_HOLD_MS) {     /* the tick counter may wrap meanwhile */
        w->watching = false;
        return KNOB_PRESS_HELD;
    }
    return KNOB_PRESS_WAIT;
}
