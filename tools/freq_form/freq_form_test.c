/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the digit the tuning step underlines in the frequency strip (src/freq_strip.c,
 *  freq_strip_step_char()) in the two forms of Settings > Display > MAIN SCREEN > Frequency (#344). For steps from
 *  1 Hz to 10 MHz (1, 2, 5 of each decade and 2.5, 6.25, 8.33, 9 and 12.5 kHz) and frequencies of 6 to 10 digits:
 *  the character underlined in "14.074.000" and in "14.074000" is the same digit of the number, the one whose place
 *  the step's first figure has, with a full mark for a step of 2-9 of it and a half one for 1; a split's first
 *  frequency is the one marked ("14.074.000 / 14.076.000"); the deck themes' text, "14.100" with ".00" or "00" after
 *  it and a 0 for the units they do not show, gives the same character; a step above the number's first digit marks
 *  nothing. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int freq_strip_step_char(const char *text, int32_t step, bool *big);

static int checks, failed;

static void check(bool ok, const char *what, const char *text, int32_t step) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s: \"%s\" step %d\n", what, text, (int)step);
    }
}

/* The digits of text up to its first space, without the dots: for each the character it is at */
static int digits_of(const char *text, int at[16]) {
    int n = 0;

    for (int i = 0; text[i] && text[i] != ' '; i++)
        if (text[i] != '.') at[n++] = i;
    return n;
}

/* The mark in text is on the digit of the step's decade, counted from the units; the character it is at, -1 none */
static int one(const char *text, int32_t step) {
    int     at[16], n = digits_of(text, at), place = 0;
    bool    big   = false;
    int32_t first = step;

    for (; first >= 10; first /= 10) place++;

    int pos = freq_strip_step_char(text, step, &big);

    if (place >= n) {
        check(pos == -1, "a step above the number marks nothing", text, step);
        return pos;
    }
    check(pos == at[n - 1 - place], "the digit of the step's decade", text, step);
    check(big == (first > 1), "a full mark for 2-9, a half one for 1", text, step);
    return pos;
}

int main(void) {
    static const int32_t steps[] = {1,     2,     5,     10,     20,     50,     100,     200,    500,
                                    1000,  2000,  2500,  5000,   6250,   8330,   9000,    10000,  12500,
                                    20000, 50000, 100000, 200000, 500000, 1000000, 10000000};
    static const int32_t freqs[] = {135700,   472000,    1810000,   7074000,   14074000,
                                    28074999, 50313000,  144174000, 432100000, 1296100000};
    char                 two[48], one_dot[48], text[64];
    bool                 big;

    for (size_t f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
        int mhz = freqs[f] / 1000000, khz = freqs[f] / 1000 % 1000, hz = freqs[f] % 1000;

        snprintf(two, sizeof(two), "%i.%03i.%03i", mhz, khz, hz);
        snprintf(one_dot, sizeof(one_dot), "%i.%03i%03i", mhz, khz, hz);
        for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
            int p2 = one(two, steps[s]), p1 = one(one_dot, steps[s]);

            check((p2 < 0) == (p1 < 0) && (p2 < 0 || two[p2] == one_dot[p1]), "the two forms mark the same digit",
                  two, steps[s]);

            /* a split: the first frequency */
            snprintf(text, sizeof(text), "%s / %i.%03i.%03i", two, mhz, (khz + 2) % 1000, hz);
            check(freq_strip_step_char(text, steps[s], &big) == p2, "a split marks its first frequency", text,
                  steps[s]);
            snprintf(text, sizeof(text), "%s / %i.%03i%03i", one_dot, mhz, (khz + 2) % 1000, hz);
            check(freq_strip_step_char(text, steps[s], &big) == p1, "a split marks its first frequency", text,
                  steps[s]);

            /* the deck themes: "14.100" and ".00" or "00", a 0 for the units */
            snprintf(text, sizeof(text), "%i.%03i.%02i0", mhz, khz, hz / 10);
            check(freq_strip_step_char(text, steps[s], &big) == p2, "the deck themes' text, two dots", text, steps[s]);
            snprintf(text, sizeof(text), "%i.%03i%02i0", mhz, khz, hz / 10);
            check(freq_strip_step_char(text, steps[s], &big) == p1, "the deck themes' text, one dot", text, steps[s]);
        }
    }

    /* by hand: 1 kHz is the 4 of 074, 500 Hz the first 0 after it, 10 Hz the middle 0 */
    check(freq_strip_step_char("14.074.000", 1000, &big) == 5 && !big, "1 kHz", "14.074.000", 1000);
    check(freq_strip_step_char("14.074000", 1000, &big) == 5 && !big, "1 kHz", "14.074000", 1000);
    check(freq_strip_step_char("14.074.000", 500, &big) == 7 && big, "500 Hz", "14.074.000", 500);
    check(freq_strip_step_char("14.074000", 500, &big) == 6 && big, "500 Hz", "14.074000", 500);
    check(freq_strip_step_char("14.074.000", 10, &big) == 8 && !big, "10 Hz", "14.074.000", 10);
    check(freq_strip_step_char("14.074000", 10, &big) == 7 && !big, "10 Hz", "14.074000", 10);
    check(freq_strip_step_char("7.074000", 10000000, &big) == -1, "10 MHz", "7.074000", 10000000);

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
