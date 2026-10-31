/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the rules of the MSG > CW beacon (cw_beacon.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "cw_beacon.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "cw_decoder/morse_code.h"

/* The windows the QRSS grabbers watch (30 m from QRP Labs' QRSS kit notes, which aim at its middle; 80 m from
 * M0PWX's QRSS pages): 10.140000-10.140100 and 3.569800-3.570000 MHz, their receivers in USB on 10.138700 and
 * 3.568600 MHz */
const qrss_band_t qrss_bands[QRSS_BANDS] = {
    {"30 m", 10100000, 10150000, 10140000, 10140100, 10140050, 10138700},
    {"80 m",  3500000,  4000000,  3569800,  3570000,  3569900,  3568600},
};

/* The table's first entry for the start of str, as the keyer takes it (cw_encoder.c get_morse): its length, 0 none */
static size_t morse_of(const char *str, const char **morse) {
    for (const cw_characters_t *c = cw_characters; c->morse; c++) {
        size_t len = strlen(c->character);

        if (strncasecmp(c->character, str, len) == 0) {
            *morse = c->morse;
            return len;
        }
    }
    return 0;
}

int qrss3_plan(const char *text, cw_el_t *out, int max) {
    int  n    = 0;
    bool word = false;  /* a word space before the next character */

    for (; text && *text;) {
        const char *morse;
        size_t      len = morse_of(text, &morse);

        if (!len) {
            word = n > 0;
            text++;
            continue;
        }
        if (n > 0 && out && n - 1 < max) {
            out[n - 1].off_ms = word ? QRSS3_WORD_MS : QRSS3_DASH_MS;
        }
        for (const char *m = morse; *m; m++, n++) {
            if (out && n < max) {
                out[n].on_ms  = *m == '-' ? QRSS3_DASH_MS : QRSS3_DOT_MS;
                out[n].off_ms = m[1] ? QRSS3_DOT_MS : 0;
            }
        }
        word = false;
        text += len;
    }
    return n;
}

uint32_t cw_plan_ms(const cw_el_t *el, int n) {
    uint32_t ms = 0;

    for (int i = 0; i < n; i++) {
        ms += el[i].on_ms + el[i].off_ms;
    }
    return ms;
}

int qrss_band_of(int32_t hz) {
    for (int i = 0; i < QRSS_BANDS; i++) {
        if (hz >= qrss_bands[i].lo_hz && hz <= qrss_bands[i].hi_hz) {
            return i;
        }
    }
    return -1;
}

int32_t qrss_freq(int band, int32_t chosen) {
    const qrss_band_t *b = &qrss_bands[band];

    if (chosen < b->lo_hz || chosen > b->hi_hz) {
        return b->freq_hz;
    }
    return chosen;
}

int32_t qrss_freq_step(int band, int32_t chosen, int steps) {
    const qrss_band_t *b = &qrss_bands[band];
    int32_t            f = qrss_freq(band, chosen) + steps * QRSS_STEP_HZ;

    if (f < b->lo_hz) {
        f = b->lo_hz;
    } else if (f > b->hi_hz) {
        f = b->hi_hz;
    }
    return f == b->freq_hz ? 0 : f;
}
