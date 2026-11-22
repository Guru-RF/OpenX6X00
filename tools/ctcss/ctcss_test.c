/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the CTCSS tone (src/ctcss.c)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "ctcss.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100

static int fails, checks;

static void check(int ok, const char *fmt, ...) {
    va_list ap;

    checks++;
    if (ok)
        return;
    fails++;
    va_start(ap, fmt);
    printf("FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* The peak of the part of x at hz, by correlation over n samples */
static double level_at(const int16_t *x, size_t n, double hz) {
    double re = 0, im = 0;

    for (size_t i = 0; i < n; i++) {
        double w = 2.0 * M_PI * hz * i / RATE;

        re += x[i] * cos(w);
        im += x[i] * sin(w);
    }
    return 2.0 * sqrt(re * re + im * im) / n;
}

int main(void) {
    static int16_t buf[RATE * 2], ref[RATE * 2];
    ctcss_t        c;

    /* The table and the steps */
    check(ctcss_count() == 50, "50 tones, not %d", ctcss_count());
    check(ctcss_tone(0) == 670 && ctcss_tone(49) == 2541, "67.0 Hz first, 254.1 Hz last");
    for (int i = 1; i < ctcss_count(); i++)
        check(ctcss_tone(i) > ctcss_tone(i - 1), "tone %d above the one before", i);
    check(ctcss_valid(915) == 915 && ctcss_valid(1230) == 1230, "91.5 and 123.0 Hz are tones");
    check(ctcss_valid(900) == CTCSS_OFF && ctcss_valid(-5) == CTCSS_OFF && ctcss_valid(0) == CTCSS_OFF,
          "what is no tone is off");
    check(ctcss_step(CTCSS_OFF, 1) == 670, "up from Off: 67.0 Hz");
    check(ctcss_step(CTCSS_OFF, -1) == 2541, "down from Off: 254.1 Hz");
    check(ctcss_step(2541, 1) == CTCSS_OFF && ctcss_step(670, -1) == CTCSS_OFF, "Off beyond both ends");
    check(ctcss_step(885, 1) == 915 && ctcss_step(948, -1) == 915, "88.5 up and 94.8 down: 91.5 Hz");
    check(ctcss_step(12345, 1) == 670, "a value that is no tone steps as Off");

    int t = CTCSS_OFF, seen = 0;

    do {
        t = ctcss_step(t, 1);
        seen++;
    } while (t != CTCSS_OFF && seen < 100);
    check(seen == 51, "up through all 50 and back to Off in 51 presses, not %d", seen);

    /* The measured levels, and each tone lower than the one below it (the transmitter lifts the higher audio) */
    check(fabsf(ctcss_level_db(915) + 22.5f) < 0.01f, "91.5 Hz at -22.5 dBFS, not %.2f", ctcss_level_db(915));
    check(fabsf(ctcss_level_db(1230) + 24.0f) < 0.01f, "123.0 Hz at -24.0 dBFS, not %.2f", ctcss_level_db(1230));
    for (int i = 1; i < ctcss_count(); i++) {
        check(ctcss_level_db(ctcss_tone(i)) < ctcss_level_db(ctcss_tone(i - 1)), "tone %d's level under the one before",
              i);
    }
    check(ctcss_level_db(670) < -18.0f && ctcss_level_db(2541) > -32.0f,
          "all levels between -32 and -18 dBFS (%.1f, %.1f)", ctcss_level_db(670), ctcss_level_db(2541));

    /* Each tone as the samples show it: its frequency and level, in blocks of an odd size (no click at a block's
     * edge: nothing beside the tone) */
    for (int i = 0; i < ctcss_count(); i++) {
        int    tone = ctcss_tone(i);
        double hz = tone / 10.0, want = 32767.0 * pow(10.0, ctcss_level_db(tone) / 20.0);
        size_t n = sizeof(buf) / sizeof(buf[0]);

        memset(buf, 0, sizeof(buf));
        ctcss_init(&c, tone, RATE);
        for (size_t at = 0; at < n;) {
            size_t k = n - at > 883 ? 883 : n - at;

            ctcss_mix(&c, buf + at, k);
            at += k;
        }

        double got = level_at(buf, n, hz), power = 0;

        for (size_t j = 0; j < n; j++)
            power += (double)buf[j] * buf[j];
        check(fabs(20.0 * log10(got / want)) < 0.1, "%.1f Hz: level %.0f, wanted %.0f", hz, got, want);
        /* all of the power is the tone's: a sine's mean square is half its peak squared */
        check(fabs(power / n / (want * want / 2.0) - 1.0) < 0.02, "%.1f Hz: only the tone (power %.3f of a sine's)", hz,
              power / n / (want * want / 2.0));
        check(level_at(buf, n, hz * 1.06) < want * 0.05, "%.1f Hz: nothing 6 %% higher", hz);
    }

    /* Audio under the tone: the tone added, the audio as it was */
    for (size_t j = 0; j < sizeof(buf) / sizeof(buf[0]); j++) {
        ref[j] = (int16_t)(8000.0 * sin(2.0 * M_PI * 1000.0 * j / RATE));
        buf[j] = ref[j];
    }
    ctcss_init(&c, 1230, RATE);
    ctcss_mix(&c, buf, sizeof(buf) / sizeof(buf[0]));
    check(fabs(level_at(buf, sizeof(buf) / sizeof(buf[0]), 1000.0) - 8000.0) < 20.0, "the 1 kHz audio keeps its level");
    check(fabs(level_at(buf, sizeof(buf) / sizeof(buf[0]), 123.0) / (32767.0 * pow(10.0, -24.0 / 20.0)) - 1.0) < 0.02,
          "123.0 Hz under it at its own level");

    /* Full-scale audio: clipped, never wrapped */
    for (size_t j = 0; j < 1000; j++)
        buf[j] = j % 2 ? 32767 : -32768;
    ctcss_init(&c, 670, RATE);
    ctcss_mix(&c, buf, 1000);
    int wrapped = 0;

    /* the tone takes at most its own peak off a full-scale sample; a wrap would change its sign */
    for (size_t j = 0; j < 1000; j++)
        wrapped += j % 2 ? buf[j] < 32767 - 4000 : buf[j] > -32767 + 4000;
    check(!wrapped, "full-scale audio stays near full scale, its sign kept");

    /* Off, and a value that is no tone: nothing added */
    memcpy(buf, ref, sizeof(buf));
    ctcss_init(&c, CTCSS_OFF, RATE);
    ctcss_mix(&c, buf, sizeof(buf) / sizeof(buf[0]));
    check(memcmp(buf, ref, sizeof(buf)) == 0, "Off: the audio untouched");
    ctcss_init(&c, 900, RATE);
    ctcss_mix(&c, buf, sizeof(buf) / sizeof(buf[0]));
    check(memcmp(buf, ref, sizeof(buf)) == 0, "90.0 Hz is no tone: the audio untouched");

    printf("ctcss_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
