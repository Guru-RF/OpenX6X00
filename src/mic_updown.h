/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - what the hand mic's UP and DOWN keys do (#300)
 *
 *  Settings > Keys > HAND MIC KEYS > Hand mic UP/DOWN. Channels (the default, what they did up to v0.3.12): a press
 *  recalls the channel before (UP) or after (DOWN) the last one tuned. Tune: a press moves the VFO a step up (UP) or
 *  down (DOWN), as one detent of the main knob does, as on the stock firmware: one tunes along a band counting the
 *  presses, without looking, where the knob is hard to turn steadily (in a car). A long press is band up or down with
 *  either. The keys send no repeat while held (mic_keys.c: the press, the long press after a second, the release).
 *  Header only, so a host test (tools/mic_updown) can check it.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>

/* cfg_mic_updown's values */
typedef enum {
    MIC_UPDOWN_CHANNELS = 0,
    MIC_UPDOWN_TUNE,
} mic_updown_t;

typedef enum {
    MIC_UD_NOTHING = 0,     /* the press itself, the release after a long press */
    MIC_UD_CHANNEL,         /* dir -1 the channel before the last one tuned, +1 the one after */
    MIC_UD_TUNE,            /* dir detents of the main knob, +1 up */
    MIC_UD_BAND,            /* dir +1 band up, -1 down */
} mic_ud_do_t;

typedef struct {
    mic_ud_do_t what;
    int         dir;
} mic_ud_t;

/* The UP key (up) or DOWN, let go before the long press (tap) or held to it (hold), with the choice: what to do. Any
 * choice but Tune is Channels */
static inline mic_ud_t mic_updown(int choice, bool up, bool tap, bool hold) {
    mic_ud_t r = {MIC_UD_NOTHING, 0};

    if (hold) {
        r.what = MIC_UD_BAND;
        r.dir  = up ? +1 : -1;
    } else if (tap && choice == MIC_UPDOWN_TUNE) {
        r.what = MIC_UD_TUNE;
        r.dir  = up ? +1 : -1;
    } else if (tap) {
        r.what = MIC_UD_CHANNEL;
        r.dir  = up ? -1 : +1;      /* UP goes up the list of channels */
    }
    return r;
}
