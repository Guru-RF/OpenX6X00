/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the frequency lock's two holders, and which of them is kept across a restart (#342)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

/* The dial is locked while either holds it: the user (FST held, the LOCK key, the Panel's LOCK, a key given
 * Frequency lock in Settings > Keys), shown by LOCK in the frequency strip, or an open app (FT8, WSPR, SSTV on the
 * air ...), whose hold ends with the app and leaves the user's lock as it was. The user's lock is a setting
 * (dial_lock, settings_manager.h): a radio switched off locked starts locked. An app's hold never is: it is not
 * written, and a start has none. Only the header, so the host test (tools/dial_lock) takes it alone. */

#pragma once

#include <stdbool.h>

typedef struct {
    bool user;      /* the user's lock: the one that is saved */
    bool app;       /* an open app's hold: never saved */
} dial_lock_t;

/* The dial does not tune */
static inline bool dial_lock_on(const dial_lock_t *l) {
    return l->user || l->app;
}

/* A start: the user's lock as it was saved, no app open yet */
static inline void dial_lock_start(dial_lock_t *l, int saved) {
    l->user = saved != 0;
    l->app  = false;
}

/* What the setting holds: the user's lock alone, whatever an app holds meanwhile */
static inline int dial_lock_saved(const dial_lock_t *l) {
    return l->user ? 1 : 0;
}
