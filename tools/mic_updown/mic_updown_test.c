/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of what the hand mic's UP and DOWN keys do (src/mic_updown.h, #300), for each event of a
 *  press as mic_keys.c sends them (the press, the release before the long press: a tap, the long press, the release
 *  after it):
 *  - Channels (0, the default): a tap of UP the channel before the last one tuned, of DOWN the one after, as up to
 *    v0.3.12;
 *  - Tune (1): a tap of UP one detent of the main knob up, of DOWN one down;
 *  - both: held, band up (UP) or down (DOWN); the press and the release after the long press nothing;
 *  - a choice the setting cannot hold (a value of a later version): Channels.
 *  See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "mic_updown.h"

#include <stdio.h>

static int checks, failed;

static void check(int choice, bool up, bool tap, bool hold, mic_ud_do_t what, int dir, const char *name) {
    mic_ud_t r = mic_updown(choice, up, tap, hold);

    checks++;
    if (r.what != what || r.dir != dir) {
        failed++;
        printf("FAIL %s: %d %d, wanted %d %d\n", name, r.what, r.dir, what, dir);
    }
}

/* Every event of a press of UP or DOWN with the choice: a tap gives tap_what, tap_dir_up for UP (DOWN the other way) */
static void key_events(int choice, mic_ud_do_t tap_what, int tap_dir_up, const char *name) {
    char what[96];

    for (int up = 0; up <= 1; up++) {
        const char *key = up ? "UP" : "DOWN";
        int         dir = up ? tap_dir_up : -tap_dir_up;

        snprintf(what, sizeof(what), "%s %s pressed", name, key);
        check(choice, up, false, false, MIC_UD_NOTHING, 0, what);
        snprintf(what, sizeof(what), "%s %s tapped", name, key);
        check(choice, up, true, false, tap_what, dir, what);
        snprintf(what, sizeof(what), "%s %s held", name, key);
        check(choice, up, false, true, MIC_UD_BAND, up ? +1 : -1, what);
        snprintf(what, sizeof(what), "%s %s let go after the long press", name, key);
        check(choice, up, false, false, MIC_UD_NOTHING, 0, what);
    }
}

int main(void) {
    key_events(MIC_UPDOWN_CHANNELS, MIC_UD_CHANNEL, -1, "Channels");
    key_events(MIC_UPDOWN_TUNE, MIC_UD_TUNE, +1, "Tune");
    key_events(2, MIC_UD_CHANNEL, -1, "a later version's 2");
    key_events(-1, MIC_UD_CHANNEL, -1, "-1");

    printf("mic_updown: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
