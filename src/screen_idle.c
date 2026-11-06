/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - when the screensaver comes and when the screen goes dark (see screen_idle.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "screen_idle.h"

#define MIN_MS 60000ull

/* ms from then to now; 0 for a then not yet come */
static uint64_t since(uint64_t then, uint64_t now) {
    return now > then ? now - then : 0;
}

/* A screensaver that may come: one chosen, and the screen not always off */
static bool saver_on(const screen_idle_t *s) {
    return s->saver != SCREEN_SAVER_NONE && !s->always_off;
}

void screen_idle_start(screen_idle_t *s, uint64_t now_ms) {
    s->power_off = false;
    s->use_ms    = now_ms;
    s->shown_ms  = now_ms;
    s->stage     = s->always_off ? SCREEN_DARK : SCREEN_LIT;
}

bool screen_idle_use(screen_idle_t *s, uint64_t now_ms) {
    if (s->stage == SCREEN_DARK && (s->always_off || s->power_off)) {
        return false;       /* dark on purpose: the keys work in the dark */
    }
    bool woke = s->stage != SCREEN_LIT;

    s->stage = SCREEN_LIT;
    if (!s->always_off) {
        s->use_ms = now_ms; /* always off: POWER's minute is not stretched by the keys, as before */
    }
    return woke;
}

void screen_idle_tx(screen_idle_t *s, bool on, uint64_t now_ms) {
    s->tx = on;
    if (!saver_on(s)) {
        return;             /* without a screensaver TX changes nothing, as before */
    }
    if (s->stage == SCREEN_SAVER) {
        s->stage = SCREEN_LIT;
    }
    if (s->stage == SCREEN_LIT) {
        s->use_ms = now_ms;
    }
}

void screen_idle_viewers(screen_idle_t *s, bool any) {
    s->viewers = any;
    if (any && s->stage == SCREEN_SAVER) {
        s->stage = SCREEN_LIT;  /* not a use: the minutes go on, the dark comes at its time */
    }
}

void screen_idle_power(screen_idle_t *s, uint64_t now_ms) {
    if (s->stage == SCREEN_DARK) {
        s->stage     = SCREEN_LIT;
        s->power_off = false;
        s->use_ms    = now_ms;
        s->shown_ms  = now_ms;
    } else {
        s->stage     = SCREEN_DARK;
        s->power_off = true;
    }
}

void screen_idle_settings(screen_idle_t *s, uint64_t now_ms) {
    if (s->always_off) {
        s->stage = SCREEN_DARK;
        return;
    }
    if (s->stage != SCREEN_DARK || !s->power_off) {
        s->stage = SCREEN_LIT;  /* dark by POWER stays dark */
    }
    s->use_ms = now_ms;
}

void screen_idle_tick(screen_idle_t *s, uint64_t now_ms) {
    if (s->stage == SCREEN_DARK) {
        return;
    }
    if (s->always_off) {
        /* POWER showed it: dark again after the screen off time, a minute at least */
        if (since(s->shown_ms, now_ms) >= (s->off_min ? s->off_min : 1) * MIN_MS) {
            s->stage = SCREEN_DARK;
        }
        return;
    }
    if (saver_on(s) && s->tx) {
        s->use_ms = now_ms; /* on the air: no screensaver, the minutes from the end of TX */
    }
    uint64_t idle = since(s->use_ms, now_ms);

    if (s->off_min && idle >= s->off_min * MIN_MS) {
        s->stage = SCREEN_DARK;
    } else if (s->stage == SCREEN_LIT && saver_on(s) && !s->viewers &&
               idle >= (s->saver_min ? s->saver_min : 1) * MIN_MS) {
        s->stage = SCREEN_SAVER;
    }
}
