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

bool key_limit_over(uint32_t now_ms, uint32_t down_ms, uint32_t limit_ms) {
    return limit_ms && down_ms && (int32_t)(now_ms - down_ms) > (int32_t)limit_ms;
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

/* ---- The schedule --------------------------------------------------------------------------------------------- */

#define DAY_MIN (24 * 60)

int bcn_min_of(time_t t) {
    struct tm tm;

    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

/* min:00 on the day of tm, days later; mktime finds the summer time itself (tm_isdst -1) */
static time_t at_min(struct tm tm, int min, int days) {
    tm.tm_mday += days;
    tm.tm_hour  = min / 60;
    tm.tm_min   = min % 60;
    tm.tm_sec   = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

time_t bcn_next(time_t now, int min) {
    struct tm tm;
    time_t    t;

    localtime_r(&now, &tm);
    if (tm.tm_hour * 60 + tm.tm_min == min) {
        return now;
    }
    t = at_min(tm, min, 0);
    return t > now ? t : at_min(tm, min, 1);
}

time_t bcn_start_at(const bcn_sched_t *s, time_t now) {
    return s->start_min < 0 ? now : bcn_next(now, s->start_min);
}

time_t bcn_end_at(const bcn_sched_t *s, time_t start) {
    struct tm tm;
    time_t    t;

    if (s->until != BCN_UNTIL_TIME || s->end_min < 0) {
        return 0;
    }
    localtime_r(&start, &tm);
    t = at_min(tm, s->end_min, 0);
    return t > start ? t : at_min(tm, s->end_min, 1);
}

time_t bcn_after_send(const bcn_sched_t *s, time_t end, int done, time_t now, int pause_s) {
    time_t next = now + pause_s;

    if (s->until == BCN_UNTIL_SENDS && done >= s->sends) {
        return 0;
    }
    if (s->until == BCN_UNTIL_TIME && end && next >= end) {
        return 0;
    }
    return next;
}

/* Position p of n round, moved by steps */
static int round_step(int p, int steps, int n) {
    return ((p + steps) % n + n) % n;
}

/* The first step after min */
static int first_after(int min) {
    return (min / BCN_STEP_MIN + 1) * BCN_STEP_MIN % DAY_MIN;
}

int bcn_step_start(int cur, int steps, int now_min) {
    int t0 = first_after(now_min);
    int n  = DAY_MIN / BCN_STEP_MIN + 1;    /* at once, then the times */
    int p  = cur < 0 ? 0 : 1 + (cur - t0 + DAY_MIN) % DAY_MIN / BCN_STEP_MIN;

    p = round_step(p, steps, n);
    return p == 0 ? BCN_NOW : (t0 + (p - 1) * BCN_STEP_MIN) % DAY_MIN;
}

int bcn_step_end(int cur, int steps, int from_min) {
    int t0 = first_after(from_min);
    int n  = DAY_MIN / BCN_STEP_MIN;
    int p  = cur < 0 ? 0 : (cur - t0 + DAY_MIN) % DAY_MIN / BCN_STEP_MIN;

    return (t0 + round_step(p, steps, n) * BCN_STEP_MIN) % DAY_MIN;
}

int bcn_step_sends(int cur, int steps) {
    int n = cur + steps;

    return n < 1 ? 1 : n > BCN_SENDS_MAX ? BCN_SENDS_MAX : n;
}
