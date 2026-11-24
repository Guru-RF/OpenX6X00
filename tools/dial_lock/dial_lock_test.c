/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the frequency lock kept across a restart (src/dial_lock.h, #342): a radio switched off
 *  with the user's lock on starts locked, one switched off unlocked starts unlocked; an app's hold of the dial is
 *  never what is saved, with or without the user's lock under it, and no start has one; the dial is locked while
 *  either holds it. The setting itself and LOCK in the frequency strip are screen_main.c's: not run here. See the
 *  Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "dial_lock.h"

#include <stdio.h>

static int checks, failed;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

int main(void) {
    dial_lock_t l = {.user = true, .app = true};    /* whatever was there */

    /* A first start: nothing saved */
    dial_lock_start(&l, 0);
    check(!l.user && !l.app && !dial_lock_on(&l), "nothing saved: the start is unlocked");
    check(dial_lock_saved(&l) == 0, "unlocked: 0 is saved");

    /* The user locks, the radio goes off and on */
    l.user = true;
    check(dial_lock_on(&l) && dial_lock_saved(&l) == 1, "the user's lock: locked, 1 is saved");
    dial_lock_start(&l, dial_lock_saved(&l));
    check(l.user && !l.app && dial_lock_on(&l), "switched off locked: the start is locked, the user's");

    /* Unlocked again, off and on */
    l.user = false;
    check(!dial_lock_on(&l) && dial_lock_saved(&l) == 0, "unlocked: 0 is saved");
    dial_lock_start(&l, dial_lock_saved(&l));
    check(!l.user && !dial_lock_on(&l), "switched off unlocked: the start is unlocked");

    /* An app holds the dial, the user has not locked it: locked now, nothing to save, the next start free */
    l.app = true;
    check(dial_lock_on(&l), "an app's hold: the dial is locked");
    check(dial_lock_saved(&l) == 0, "an app's hold alone is not saved");
    dial_lock_start(&l, dial_lock_saved(&l));
    check(!l.user && !l.app && !dial_lock_on(&l), "switched off with an app holding the dial: the start is unlocked");

    /* The user's lock under an app's hold: the user's is saved; the app ends, the lock stays; the user unlocks while
     * the app holds, nothing is saved though the dial is still locked */
    l.user = true;
    l.app  = true;
    check(dial_lock_saved(&l) == 1, "the user's lock under an app's hold: 1 is saved");
    l.app = false;
    check(dial_lock_on(&l) && dial_lock_saved(&l) == 1, "the app ends: the user's lock stays, and stays saved");
    l.app  = true;
    l.user = false;
    check(dial_lock_on(&l) && dial_lock_saved(&l) == 0, "the user unlocks under an app's hold: locked, 0 is saved");

    /* Any value but 0 in the setting is a lock; a start never has an app's hold */
    l.app = true;
    dial_lock_start(&l, 7);
    check(l.user && !l.app, "a start: the saved lock, no app's hold");

    printf("dial_lock: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
