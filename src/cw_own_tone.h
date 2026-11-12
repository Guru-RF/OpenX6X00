/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's own CW sidetone for the decoder and the CW Trainer (#198, #199)
 *
 *  While the X6200's base keys in CW (KEY Train or a transmission) it cuts the receiver's audio to the processor
 *  and puts its sidetone there instead, at the KEY Volume's level, the Moni level not counting. Measured on our
 *  X6200 (base 1.0.8) at 700 Hz: -89 dBFS at Volume 10 (the default), -66 at 40, -48 at 63, about -96.8 + 0.77 dB a
 *  step, with the cut audio at -108. The X6100's base puts it in at full scale. The decoder's noise reference, taken
 *  from the band before the key-down, follows the audio down by 1 % a frame (tone_finder.cpp), about 9 s for the
 *  50-odd dB from a band's noise to Volume 10's sidetone: the decoder and the Trainer read nothing of the radio's
 *  own keying, and at Volume 63 lost its first dits. While the base keys, dsp.cpp raises the decoder's audio by
 *  cw_own_tone_gain() to put the sidetone near CW_OWN_TONE_DB, and limits it there: a block of the band's audio
 *  coming back as the base unkeys stays the size of a mark.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <math.h>
#include <stdint.h>

#define CW_OWN_TONE_DB      -30.0f     /* where the sidetone goes, dBFS */
#define CW_OWN_TONE_LIMIT   0.1f       /* -20 dBFS: the most it lets through */
#define CW_OWN_GAIN_MAX_DB  60.0f      /* Volume 0, no sidetone: its silence not raised into noise */

/* The sidetone's level in the processor's audio, dBFS, at KEY Volume key_vol (0..63) */
static inline float cw_own_tone_db(int32_t key_vol) {
    return -96.8f + 0.77f * (float)key_vol;
}

/* The factor that puts it at CW_OWN_TONE_DB: never under 1, never over CW_OWN_GAIN_MAX_DB */
static inline float cw_own_tone_gain(int32_t key_vol) {
    float db = CW_OWN_TONE_DB - cw_own_tone_db(key_vol);

    if (db < 0.0f) db = 0.0f;
    if (db > CW_OWN_GAIN_MAX_DB) db = CW_OWN_GAIN_MAX_DB;
    return powf(10.0f, db / 20.0f);
}

/* One sample of the decoder's audio (after the DC blocker: the codec's offset is not raised) */
static inline float cw_own_tone_apply(float x, float gain) {
    x *= gain;
    return x > CW_OWN_TONE_LIMIT ? CW_OWN_TONE_LIMIT : x < -CW_OWN_TONE_LIMIT ? -CW_OWN_TONE_LIMIT : x;
}
