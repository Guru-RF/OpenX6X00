/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/closeup_est.c: the close-up's "off by" estimate. A carrier 150 Hz off read as -150, nothing in
 *  the passband no estimate, a CW signal at its pitch on target, a flat (SSB-like) block off by 150 read as such,
 *  a reset forgetting the old signal, a stronger signal outside the passband ignored; in SSB a voice whose
 *  carrier is 150 Hz low read as -150 from its edge (USB: the lower, LSB: the upper), however its energy lies.
 *    closeup_est_test
 */

#include "closeup_est.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define N       256
#define SPAN    2000

static int fails, checks;

static void check(int ok, const char *what, double v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %.1f\n", what, v);
    }
}

static float row[N];

static void noise(void) {
    for (int i = 0; i < N; i++) row[i] = -100.0f + (rand() % 400) / 100.0f - 2.0f;
}

static int bin_of(float hz) {
    return (int) lroundf(hz * N / SPAN + N / 2);
}

static void carrier(float hz, float db) {
    int b = bin_of(hz);

    row[b] = db;
    row[b - 1] = row[b + 1] = db - 8.0f;
}

static void block(float lo, float hi, float db) {
    for (int i = bin_of(lo); i <= bin_of(hi); i++) row[i] = db + (rand() % 200) / 100.0f - 1.0f;
}

/* A voice: 300 to 2700 Hz of audio, strongest about 900, from a carrier at carrier_hz, up (USB) or down (LSB), in
 * a row of span Hz */
static void voice(float carrier_hz, int dir, float db, float span) {
    for (int a = 300; a <= 2700; a += 4) {
        int   b = (int) lroundf((carrier_hz + dir * a) * N / span + N / 2);
        float d = db - 20.0f * (a - 900.0f) * (a - 900.0f) / (1800.0f * 1800.0f) + (rand() % 400) / 100.0f - 2.0f;

        if (b >= 0 && b < N && d > row[b]) row[b] = d;
    }
}

/* The row spans 4 kHz here: the voice and its passband, 50 to 2950 Hz from the dial at -1500 (the row's centre is
 * the passband's middle) */
static void usb_edge(void) {
    closeup_est_t e;

    closeup_est_reset(&e);
    for (int k = 0; k < 20; k++) {
        noise();
        voice(-1500 - 150, 1, -60, 4000);
        closeup_est_update(&e, row, N, 4000, -1450, 1450, -1500 + 300, 1);
    }
    check(e.valid && fabsf(e.off_hz + 150) <= 40, "usb_edge", e.off_hz);
}

/* The same voice through the DSP's FFT window (Blackman-Harris: -3 dB a bin out, -23 three bins out, an edge of
 * a strong voice spread 150 Hz at 49 Hz a bin), on a 12.5 kHz row */
static void usb_edge_smeared(void) {
    closeup_est_t e;
    float         lin[N], sm[N];

    closeup_est_reset(&e);
    for (int k = 0; k < 20; k++) {
        noise();
        voice(-1500 - 150, 1, -60, 12500);
        for (int i = 0; i < N; i++) lin[i] = powf(10.0f, row[i] / 10.0f);
        for (int i = 0; i < N; i++) {
            float sum = 0.0f, w = 0.0f;

            static const float kernel[] = {1.0f, 0.5f, 0.08f, 0.005f, 0.0005f};   /* the BH4 window's, in power */

            for (int j = -4; j <= 4; j++) {
                float c = kernel[j < 0 ? -j : j];

                if (i + j >= 0 && i + j < N) sum += c * lin[i + j];
                w += c;
            }
            sm[i] = 10.0f * log10f(sum / w);
        }
        closeup_est_update(&e, sm, N, 12500, -1450, 1450, -1500 + 300, 1);
    }
    /* within a bin: at 49 Hz a bin the voice's own first bin is already 24 Hz under its edge */
    check(e.valid && fabsf(e.off_hz + 150) <= 60, "usb_edge_smeared", e.off_hz);
}

static void lsb_edge(void) {
    closeup_est_t e;

    closeup_est_reset(&e);
    for (int k = 0; k < 20; k++) {
        noise();
        voice(1500 - 150, -1, -60, 4000);
        closeup_est_update(&e, row, N, 4000, -1450, 1450, 1500 - 300, -1);
    }
    check(e.valid && fabsf(e.off_hz + 150) <= 40, "lsb_edge", e.off_hz);
}

/* Noise as a row of the close-up has it: 1 or 2 FFTs, a bin's power exponential (peaks 10 dB and more) */
static void raw_noise(void) {
    for (int i = 0; i < N; i++) {
        float u = (rand() + 1.0f) / ((float) RAND_MAX + 2.0f), v = (rand() + 1.0f) / ((float) RAND_MAX + 2.0f);

        row[i] = -100.0f + 10.0f * log10f((-logf(u) - logf(v)) / 2.0f);
    }
}

/* After a retune, an empty band: no label on the first rows either (they were valid on most of them) */
static void raw_noise_after_reset(void) {
    closeup_est_t e;
    int           shown = 0;

    for (int t = 0; t < 50; t++) {
        closeup_est_reset(&e);
        for (int k = 0; k < 5; k++) {
            raw_noise();
            closeup_est_update(&e, row, N, SPAN, -1000, 1000, 0, 0);
            shown += e.valid;
        }
    }
    check(shown == 0, "raw_noise_after_reset", shown);
}

/* Rows of another width (the span switched live): the average from them alone, not mixed with the old bins */
static void span_change_restarts(void) {
    closeup_est_t e;

    closeup_est_reset(&e);
    for (int k = 0; k < 10; k++) {
        noise();
        carrier(-150, -60);
        closeup_est_update(&e, row, N, SPAN, -1000, 1000, 0, 0);
    }
    for (int k = 0; k < 4; k++) {   /* the same carrier in rows of twice the width: at -150 Hz still */
        noise();
        row[(int) lroundf(-150.0f * N / (2 * SPAN) + N / 2)] = -60.0f;
        closeup_est_update(&e, row, N, 2 * SPAN, -1000, 1000, 0, 0);
    }
    check(e.valid && fabsf(e.off_hz + 150) <= 20, "span_change_restarts", e.off_hz);
}

static void carrier_offset(void) {
    closeup_est_t e;

    closeup_est_reset(&e);
    for (int k = 0; k < 10; k++) {
        noise();
        carrier(-150, -60);
        closeup_est_update(&e, row, N, SPAN, -1000, 1000, 0, 0);
    }
    check(e.valid && fabsf(e.off_hz + 150) <= 10, "carrier_offset", e.off_hz);
}

static void noise_only_no_offset(void) {
    closeup_est_t e;

    closeup_est_reset(&e);
    for (int k = 0; k < 10; k++) {
        noise();
        closeup_est_update(&e, row, N, SPAN, -1000, 1000, 0, 0);
    }
    check(!e.valid, "noise_only_no_offset", e.off_hz);
}

static void cw_pitch_target(void) {
    closeup_est_t e;

    closeup_est_reset(&e);
    for (int k = 0; k < 10; k++) {
        noise();
        carrier(600, -60);
        closeup_est_update(&e, row, N, SPAN, 300, 900, 600, 0);
    }
    check(e.valid && fabsf(e.off_hz) <= 10, "cw_pitch_target", e.off_hz);
}

static void lsb_mirror(void) {
    closeup_est_t e;

    closeup_est_reset(&e);
    for (int k = 0; k < 10; k++) {
        noise();
        block(-900 + 150, -100 + 150, -70);
        closeup_est_update(&e, row, N, SPAN, -900, -100, -500, 0);
    }
    check(e.valid && fabsf(e.off_hz - 150) <= 60, "lsb_mirror", e.off_hz);
}

static void reset_on_retune(void) {
    closeup_est_t e;

    closeup_est_reset(&e);
    for (int k = 0; k < 10; k++) {
        noise();
        carrier(-150, -60);
        closeup_est_update(&e, row, N, SPAN, -1000, 1000, 0, 0);
    }
    closeup_est_reset(&e);
    noise();
    closeup_est_update(&e, row, N, SPAN, -1000, 1000, 0, 0);
    check(!e.valid, "reset_on_retune", e.off_hz);
}

static void outside_passband_ignored(void) {
    closeup_est_t e;

    closeup_est_reset(&e);
    for (int k = 0; k < 10; k++) {
        noise();
        carrier(900, -50);
        carrier(-100, -65);
        closeup_est_update(&e, row, N, SPAN, -500, 500, 0, 0);
    }
    check(e.valid && fabsf(e.off_hz + 100) <= 10, "outside_passband_ignored", e.off_hz);
}

int main(void) {
    srand(1);
    carrier_offset();
    noise_only_no_offset();
    cw_pitch_target();
    lsb_mirror();
    reset_on_retune();
    outside_passband_ignored();
    raw_noise_after_reset();
    span_change_restarts();
    usb_edge();
    usb_edge_smeared();
    lsb_edge();
    printf("closeup_est: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
