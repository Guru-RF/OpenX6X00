/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the internet voice apps' PTT key and audio path rules. See voice_ptt.h.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "voice_ptt.h"

vptt_key_t vptt_press(vptt_t *p, bool app_open) {
    if (p->down && p->owner == VPTT_RADIO) {
        return VPTT_RADIO;     /* the radio may still be keyed by it: never the app's on top */
    }
    p->down  = true;
    p->owner = app_open ? VPTT_APP : VPTT_RADIO;
    return p->owner;
}

vptt_key_t vptt_release(vptt_t *p) {
    vptt_key_t to = p->down ? p->owner : VPTT_RADIO;

    p->down  = false;
    p->owner = VPTT_RADIO;
    return to;
}

vpath_t vpath(vgate_t *g, bool connected, bool talk, bool playing, bool on_air, uint64_t now_ms) {
    if (on_air) {
        g->on_air = true;
        return VPATH_OFF;
    }
    if (g->on_air) {
        g->on_air     = false;
        g->air_end_ms = now_ms;
    }
    if (g->air_end_ms && now_ms - g->air_end_ms < VOICE_AIR_HOLD_MS) {
        return VPATH_OFF;
    }
    if (!connected) {
        return VPATH_OFF;
    }
    if (talk) {
        return VPATH_TALK;
    }
    return playing ? VPATH_LISTEN : VPATH_OFF;
}
