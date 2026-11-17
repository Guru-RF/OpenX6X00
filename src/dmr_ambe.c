/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - DMR's voice to sound (dmr_ambe.h).
 *
 *  A voice frame's 72 bits are four code words one after the other, most significant bit first: C0 (24 bits, Golay
 *  24,12), C1 (23 bits, Golay 23,12, scrambled with a sequence made from C0's data), C2 (11 bits) and C3 (14 bits).
 *  On the air they are interleaved 4 by 18: the frame's bit n is bit (n mod 4) * 18 + n / 4 of that row. mbelib takes
 *  the code words as ambe_fr[4][24], bit 0 the least significant, and does the FEC, the descrambling and the
 *  synthesis (mbe_processAmbe3600x2450Framef).
 *
 *  After the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico): its DMR app's decoder, the frames' places
 *  in the burst and its level control.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dmr_ambe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mbelib.h"

#define FRAME_BITS 72
#define GAIN_START 7.0f         /* mbelib's own (mbe_floattoshort) */
#define GAIN_MAX   50.0f
#define PEAK_WANT  11000.0f     /* a frame's peak about a third of full scale */
#define GAIN_RISE  0.05f        /* of the way to the gain wanted, a frame: up slowly, down at once */
#define UV_QUALITY 3            /* mbelib's unvoiced synthesis: its tools' default */

struct dmr_ambe {
    mbe_parms cur, prev, prev_enh;
    float     gain;
};

/* The four code words' lengths, in the row's order */
static const int word_len[4] = {24, 23, 11, 14};

dmr_ambe_t *dmr_ambe_new(void) {
    dmr_ambe_t *d = calloc(1, sizeof(*d));

    if (d) dmr_ambe_reset(d);
    return d;
}

void dmr_ambe_free(dmr_ambe_t *d) {
    free(d);
}

void dmr_ambe_reset(dmr_ambe_t *d) {
    mbe_initMbeParms(&d->cur, &d->prev, &d->prev_enh);
    d->gain = GAIN_START;
}

static int bit_at(const uint8_t *b, int n) {
    return (b[n >> 3] >> (7 - (n & 7))) & 1;
}

/* Where the frame's bit n is in the burst: the first frame at 0, the second split around the 48 bits of sync or
 * signalling in the middle (36 bits before them, 36 after), the third at 192 */
static int burst_pos(int f, int n) {
    if (f == 0) return n;
    if (f == 2) return 192 + n;
    return n < 36 ? 72 + n : 156 + (n - 36);
}

void dmr_ambe_frame(const uint8_t burst[DMR_BURST_BYTES], int f, char fr[4][24]) {
    memset(fr, 0, 4 * 24);
    for (int n = 0; n < FRAME_BITS; n++) {
        int s = (n % 4) * 18 + n / 4;   /* the bit's place in the row C0 C1 C2 C3 */
        int w = 0;

        while (s >= word_len[w]) s -= word_len[w++];
        fr[w][word_len[w] - 1 - s] = (char)bit_at(burst, burst_pos(f, n));
    }
}

int dmr_ambe_burst(dmr_ambe_t *d, const uint8_t burst[DMR_BURST_BYTES], int16_t pcm[DMR_BURST_PCM]) {
    int total = 0;

    for (int f = 0; f < 3; f++) {
        char  fr[4][24], ambe_d[49], err_str[64];    /* err_str: a character an error, 27 at most */
        float out[DMR_FRAME_PCM], peak = 0;
        int   errs = 0, errs2 = 0;

        dmr_ambe_frame(burst, f, fr);
        mbe_processAmbe3600x2450Framef(out, &errs, &errs2, err_str, fr, ambe_d, &d->cur, &d->prev, &d->prev_enh,
                                       UV_QUALITY);
        total += errs2;

        /* The level: a frame's peak near PEAK_WANT, without jumps. Silence leaves the gain as it was */
        for (int i = 0; i < DMR_FRAME_PCM; i++) {
            float a = fabsf(out[i]);

            if (a > peak) peak = a;
        }
        if (peak > 1.0f) {
            float want = PEAK_WANT / peak;

            if (want < d->gain) d->gain = want;
            else d->gain += (want - d->gain) * GAIN_RISE;
        }
        if (d->gain > GAIN_MAX) d->gain = GAIN_MAX;
        for (int i = 0; i < DMR_FRAME_PCM; i++) {
            float s = out[i] * d->gain;

            pcm[f * DMR_FRAME_PCM + i] = (int16_t)(s > 32000.0f ? 32000 : s < -32000.0f ? -32000 : s);
        }
    }
    return total;
}
