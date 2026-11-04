/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - CapsLock+Space, the keyboard's PTT toggle (see kbd_ptt.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "kbd_ptt.h"

/* On and still keyed: let go. Off, or on with the PTT gone since: key */
bool kbd_ptt_chord(kbd_ptt_t *p, bool ptt) {
    bool let_go = p->on && ptt;

    p->on = false;
    return !let_go;
}

void kbd_ptt_keyed(kbd_ptt_t *p, bool ok, uint64_t now_ms) {
    p->on       = ok;
    p->on_air   = false;
    p->keyed_ms = now_ms;
}

kbd_ptt_tick_t kbd_ptt_tick(kbd_ptt_t *p, bool ptt, bool tx, bool no_data, uint64_t now_ms) {
    if (!p->on) {
        return KBD_PTT_STAY;
    }
    if (!ptt) {
        p->on = false;
        return KBD_PTT_ENDED;
    }
    if (no_data || now_ms - p->keyed_ms > KBD_PTT_LIMIT_MS) {
        p->on = false;
        return no_data ? KBD_PTT_NO_DATA : KBD_PTT_TIME_UP;
    }
    if (tx) {
        p->on_air = true;
        return KBD_PTT_STAY;
    }
    if (p->on_air || now_ms - p->keyed_ms > KBD_PTT_TAKE_MS) {
        p->on = false;
        return KBD_PTT_LET_GO;
    }
    return KBD_PTT_STAY;
}

bool kbd_ptt_gone(kbd_ptt_t *p, bool ptt) {
    bool let_go = p->on && ptt;

    p->on = false;
    return let_go;
}
