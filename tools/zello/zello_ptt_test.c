/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the Zello app's two rules (src/voice_ptt.c, shared with SVXConnect): the radio's PTT key never keys the
 *  radio while the app is open and keys it again the moment the app has closed, and Zello's audio path is the radio's
 *  own while the radio transmits and half a second after. `make` in tools/zello.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "voice_ptt.h"

#include <stdio.h>
#include <stdlib.h>

static int failed, checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            failed++;                                                                   \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                               \
    } while (0)

/* The radio's PTT as screen_main.c drives it: a press the radio owns keys it, its release unkeys it */
typedef struct {
    vptt_t ptt;
    int         radio_keyed;    /* ptt_key(PRESS) without its release */
    int         zello_talks;
    int         radio_presses;
} key_world_t;

static void press(key_world_t *w, bool app_open) {
    if (vptt_press(&w->ptt, app_open) == VPTT_APP) {
        w->zello_talks = 1;
    } else {
        w->radio_keyed = 1;
        w->radio_presses++;
    }
}

static void release(key_world_t *w) {
    if (vptt_release(&w->ptt) == VPTT_APP) w->zello_talks = 0;
    else w->radio_keyed = 0;
}

static void test_ptt(void) {
    key_world_t w = {0};

    /* App closed: the key is the radio's */
    press(&w, false);
    CHECK(w.radio_keyed && !w.zello_talks);
    release(&w);
    CHECK(!w.radio_keyed);

    /* App open: Zello talks, the radio is never keyed, not even for a moment */
    press(&w, true);
    CHECK(w.zello_talks && !w.radio_keyed && w.radio_presses == 1);
    release(&w);
    CHECK(!w.zello_talks && !w.radio_keyed && w.radio_presses == 1);

    /* Several presses with the app open: none reaches the radio */
    for (int i = 0; i < 5; i++) {
        press(&w, true);
        CHECK(!w.radio_keyed);
        release(&w);
    }
    CHECK(w.radio_presses == 1);

    /* Held for Zello when the app closes: the release keys nothing, the next press keys the radio at once */
    press(&w, true);
    CHECK(w.zello_talks);
    release(&w);                    /* after the app closed: screen_main.c still asks vnet_ptt_key, it was Zello's */
    CHECK(!w.radio_keyed);
    press(&w, false);
    CHECK(w.radio_keyed && w.radio_presses == 2);
    release(&w);
    CHECK(!w.radio_keyed);

    /* Held for the radio when the app opens: still the radio's until released, so the radio is unkeyed */
    press(&w, false);
    CHECK(w.radio_keyed);
    release(&w);                    /* the app is open now: the release still goes to the radio */
    CHECK(!w.radio_keyed && !w.zello_talks);

    /* A lost release of the radio's press, then a press with the app open: stays the radio's, never both */
    press(&w, false);
    press(&w, true);
    CHECK(w.radio_keyed && !w.zello_talks);
    release(&w);
    CHECK(!w.radio_keyed);

    /* A release with no press seen goes to the radio (it unkeys nothing that is not keyed) */
    CHECK(vptt_release(&w.ptt) == VPTT_RADIO);
    CHECK(!w.ptt.down);
}

static void test_path(void) {
    vgate_t g = {0};
    uint64_t     t = 1000000;

    CHECK(vpath(&g, false, false, true, false, t) == VPATH_OFF);    /* not on a channel */
    CHECK(vpath(&g, true, false, true, false, t) == VPATH_LISTEN);
    CHECK(vpath(&g, true, true, true, false, t) == VPATH_TALK);

    /* The radio transmits, whatever keyed it: off, also with a message wanted */
    CHECK(vpath(&g, true, true, true, true, t + 10) == VPATH_OFF);
    CHECK(vpath(&g, true, false, true, true, t + 2000) == VPATH_OFF);
    /* Back on receive: off for VOICE_AIR_HOLD_MS more */
    CHECK(vpath(&g, true, false, true, false, t + 3000) == VPATH_OFF);
    CHECK(vpath(&g, true, false, true, false, t + 3000 + VOICE_AIR_HOLD_MS - 1) == VPATH_OFF);
    CHECK(vpath(&g, true, true, true, false, t + 3000 + VOICE_AIR_HOLD_MS - 1) == VPATH_OFF);
    CHECK(vpath(&g, true, false, true, false, t + 3000 + VOICE_AIR_HOLD_MS) == VPATH_LISTEN);

    /* A short key in between restarts the hold from its end */
    t += 10000;
    CHECK(vpath(&g, true, false, true, true, t) == VPATH_OFF);
    CHECK(vpath(&g, true, false, true, false, t + 40) == VPATH_OFF);
    CHECK(vpath(&g, true, false, true, true, t + 300) == VPATH_OFF);
    CHECK(vpath(&g, true, false, true, false, t + 340) == VPATH_OFF);
    CHECK(vpath(&g, true, false, true, false, t + 340 + VOICE_AIR_HOLD_MS - 1) == VPATH_OFF);
    CHECK(vpath(&g, true, false, true, false, t + 340 + VOICE_AIR_HOLD_MS) == VPATH_LISTEN);

    /* Off the channel: off whatever is wanted */
    CHECK(vpath(&g, false, true, true, false, t + 5000) == VPATH_OFF);
    /* Nothing to hear: the receiver's own path; a message from the hand mic still goes */
    CHECK(vpath(&g, true, false, false, false, t + 5000) == VPATH_OFF);
    CHECK(vpath(&g, true, true, false, false, t + 5000) == VPATH_TALK);
}

int main(void) {
    test_ptt();
    test_path();
    printf("zello_ptt_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
