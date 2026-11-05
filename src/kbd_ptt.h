/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - CapsLock+Space, the keyboard's PTT toggle (kbd_ptt.c)
 *
 *  The first chord keys the radio as its PTT key does, the next one lets go. The toggle follows the radio: it is on
 *  only while the GUI's PTT it keyed is still on, so TX ended any other way (the PTT key let go, the SWR protection,
 *  a band stop) leaves it off and the next chord keys again instead of letting go of nothing. A key that never got
 *  the radio on the air within KBD_PTT_TAKE_MS, or a radio back on receive while the PTT is still on, is let go, as
 *  TCI's: it would key later with no one expecting it. So is the keyboard going away. Nobody holds a key that ends
 *  it, so it also lets go when the base's data stops (no SWR protection, no power reading, and radio.c's flow reset
 *  never ends a TX the GUI keys) and after KBD_PTT_LIMIT_MS, as the browser's PTT and TCI's. Here only the decisions,
 *  from what the radio says; screen_main.c keys and lets go.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBD_PTT_TAKE_MS  2000           /* keyed and not on the air this long: let go */
#define KBD_PTT_LIMIT_MS (180 * 1000)   /* keyed this long: let go, as the browser's PTT (PTT_LIMIT_MS) and TCI's */

typedef struct kbd_ptt {
    bool     on;        /* keyed by the chord, not let go since */
    bool     on_air;    /* the radio was seen on the air since */
    uint64_t keyed_ms;
} kbd_ptt_t;

/* What a tick finds */
typedef enum {
    KBD_PTT_STAY,       /* off, or on and as it should be */
    KBD_PTT_ENDED,      /* the PTT went off another way: the toggle is off, nothing to let go */
    KBD_PTT_LET_GO,     /* let go of the PTT now (never on the air, or back on receive): the toggle is off */
    KBD_PTT_NO_DATA,    /* let go now: no data from the base (radio_no_data) */
    KBD_PTT_TIME_UP,    /* let go now: keyed KBD_PTT_LIMIT_MS ago */
} kbd_ptt_tick_t;

/* The chord pressed: true to key, false to let go (the toggle is then off). ptt: the GUI's PTT is on now */
bool kbd_ptt_chord(kbd_ptt_t *p, bool ptt);

/* The key's outcome: refused (ok false), the toggle stays off */
void kbd_ptt_keyed(kbd_ptt_t *p, bool ok, uint64_t now_ms);

/* While on, every so often. ptt: the GUI's PTT is on; tx: the radio is on the air; no_data: the base sends none */
kbd_ptt_tick_t kbd_ptt_tick(kbd_ptt_t *p, bool ptt, bool tx, bool no_data, uint64_t now_ms);

/* The keyboard went away: true when the PTT is to be let go. The toggle is off after it */
bool kbd_ptt_gone(kbd_ptt_t *p, bool ptt);

#ifdef __cplusplus
}
#endif
