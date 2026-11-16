/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Zello app's PTT key and audio path rules. See zello_ptt.h.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "zello_ptt.h"

zello_key_t zello_ptt_press(zello_ptt_t *p, bool app_open) {
    if (p->down && p->owner == ZELLO_KEY_RADIO) {
        return ZELLO_KEY_RADIO;     /* the radio may still be keyed by it: never Zello's on top */
    }
    p->down  = true;
    p->owner = app_open ? ZELLO_KEY_ZELLO : ZELLO_KEY_RADIO;
    return p->owner;
}

zello_key_t zello_ptt_release(zello_ptt_t *p) {
    zello_key_t to = p->down ? p->owner : ZELLO_KEY_RADIO;

    p->down  = false;
    p->owner = ZELLO_KEY_RADIO;
    return to;
}

zello_path_t zello_path(zello_gate_t *g, bool connected, bool talk, bool playing, bool on_air, uint64_t now_ms) {
    if (on_air) {
        g->on_air = true;
        return ZELLO_PATH_OFF;
    }
    if (g->on_air) {
        g->on_air     = false;
        g->air_end_ms = now_ms;
    }
    if (g->air_end_ms && now_ms - g->air_end_ms < ZELLO_AIR_HOLD_MS) {
        return ZELLO_PATH_OFF;
    }
    if (!connected) {
        return ZELLO_PATH_OFF;
    }
    if (talk) {
        return ZELLO_PATH_TALK;
    }
    return playing ? ZELLO_PATH_LISTEN : ZELLO_PATH_OFF;
}
