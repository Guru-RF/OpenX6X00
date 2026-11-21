/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - RTTY's transmitter (#312), see rtty_tx.h
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "rtty_tx.h"

#include "i18n.h"               /* N_: the presets' names */

#include <ctype.h>
#include <math.h>
#include <string.h>

const char rtty_ita2_letters[32] = {'\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\0', 'D', 'R',
                                    'J',  'N', 'F',  'C', 'K', 'T', 'Z', 'L', 'W',  'H', 'Y',
                                    'P',  'Q', 'O',  'B', 'G', '\0', 'M', 'X', 'V', '\0'};

const char rtty_ita2_figures[32] = {'\0', '3', '\n', '-', ' ', '\0', '8', '7', '\0', '$', '4',
                                    '\'', ',', '!',  ':', '(', '5',  '"', ')', '2', '#', '6',
                                    '0',  '1', '9',  '?', '&', '\0', '.', '/', ';', '\0'};

bool rtty_mark_high(int side, bool reverse) {
    return (side > 0 && !reverse) || (side < 0 && reverse);
}

void rtty_tones(uint16_t center, uint16_t shift, bool mark_high, float *mark_hz, float *space_hz) {
    float half = shift / 2.0f;

    *mark_hz  = center + (mark_high ? half : -half);
    *space_hz = center + (mark_high ? -half : half);
}

/* ---- the presets ---- */

const rtty_preset_t rtty_presets[RTTY_PRESETS] = {
    [RTTY_PRESET_WEATHER] = {N_("Weather"), 5000, 450, true},
    [RTTY_PRESET_HAM]     = {N_("Ham"), 4545, 170, false},
};

int rtty_preset_find(uint32_t rate, uint16_t shift, bool reverse) {
    for (int i = 0; i < RTTY_PRESETS; i++) {
        const rtty_preset_t *p = &rtty_presets[i];

        if (p->rate == rate && p->shift == shift && p->reverse == reverse) return i;
    }
    return -1;
}

int rtty_preset_next(int current) {
    return current == RTTY_PRESET_HAM ? RTTY_PRESET_WEATHER : RTTY_PRESET_HAM;
}

/* ---- ITA2 ---- */

void rtty_enc_init(rtty_enc_t *e) {
    e->shift = RTTY_ITA2_LTRS;
}

char rtty_enc_shown(char c) {
    if (c == '\t') return ' ';
    if (c == '\n' || c == ' ') return c;
    c = (char)toupper((unsigned char)c);
    for (int i = 0; c && i < 32; i++) {
        if (rtty_ita2_letters[i] == c || rtty_ita2_figures[i] == c) return c;
    }
    return 0;
}

int rtty_enc_char(rtty_enc_t *e, char c, uint8_t *codes) {
    char    s = rtty_enc_shown(c);
    uint8_t need;
    int     n = 0;

    if (!s) return 0;
    if (s == '\n') {
        codes[0] = RTTY_ITA2_CR;
        codes[1] = RTTY_ITA2_LF;
        return 2;
    }
    if (s == ' ') {
        codes[0] = RTTY_ITA2_SPACE;
        /* in figures a receiver may unshift on the space or not: not known any more */
        if (e->shift == RTTY_ITA2_FIGS) e->shift = 0;
        return 1;
    }
    for (int i = 0; i < 32; i++) {
        if (rtty_ita2_letters[i] == s || rtty_ita2_figures[i] == s) {
            need = rtty_ita2_letters[i] == s ? RTTY_ITA2_LTRS : RTTY_ITA2_FIGS;
            if (e->shift != need) {
                codes[n++] = need;
                e->shift = need;
            }
            codes[n++] = (uint8_t)i;
            return n;
        }
    }
    return 0;   /* never: rtty_enc_shown found it */
}

/* ---- AFSK ---- */

void rtty_afsk_init(rtty_afsk_t *a, float rate, float baud, float mark_hz, float space_hz) {
    memset(a, 0, sizeof(*a));
    a->spb = rate / baud;
    a->dphi[0] = 2.0f * (float)M_PI * space_hz / rate;
    a->dphi[1] = 2.0f * (float)M_PI * mark_hz / rate;
    a->ramp = (uint32_t)lroundf(RTTY_TX_RAMP_S * rate);
}

bool rtty_afsk_tone(rtty_afsk_t *a, bool mark, float bits) {
    int i;

    if (a->count == RTTY_AFSK_SEGS) return false;
    i = (a->head + a->count++) % RTTY_AFSK_SEGS;
    a->pos += bits * a->spb;
    a->seg[i].mark = mark;
    a->seg[i].end = (uint32_t)llround(a->pos);
    return true;
}

bool rtty_afsk_code(rtty_afsk_t *a, uint8_t code) {
    if (a->count + 7 > RTTY_AFSK_SEGS) return false;
    rtty_afsk_tone(a, false, 1.0f);
    for (int b = 0; b < 5; b++) {
        rtty_afsk_tone(a, (code >> b) & 1, 1.0f);
    }
    rtty_afsk_tone(a, true, RTTY_TX_STOP_BITS);
    return true;
}

void rtty_afsk_fade(rtty_afsk_t *a) {
    a->fade_end = (uint32_t)llround(a->pos);
}

bool rtty_afsk_busy(const rtty_afsk_t *a) {
    return a->count > 0;
}

/* Raised cosine: 0 at k = 0, 1 at k = len */
static float ramp(uint32_t k, uint32_t len) {
    return k >= len ? 1.0f : 0.5f - 0.5f * cosf((float)M_PI * (float)k / (float)len);
}

size_t rtty_afsk_render(rtty_afsk_t *a, int16_t *buf, size_t n) {
    const float two_pi = 2.0f * (float)M_PI;
    size_t      out = 0;

    while (out < n && a->count) {
        uint32_t end = a->seg[a->head].end;
        float    dphi = a->dphi[a->seg[a->head].mark];

        for (; out < n && a->n < end; a->n++) {
            float g = ramp(a->n, a->ramp);

            if (a->fade_end) {
                g *= ramp(a->fade_end > a->n ? a->fade_end - a->n - 1 : 0, a->ramp);
            }
            a->phase += dphi;
            if (a->phase >= two_pi) a->phase -= two_pi;
            buf[out++] = (int16_t)lrintf(sinf(a->phase) * RTTY_TX_AMP * g);
        }
        if (a->n >= end) {
            a->head = (a->head + 1) % RTTY_AFSK_SEGS;
            a->count--;
        }
    }
    return out;
}

/* ---- a transmission ---- */

enum { STAGE_START, STAGE_TEXT, STAGE_DONE };

void rtty_tx_start(rtty_tx_t *t, const rtty_tx_cfg_t *cfg, rtty_tx_next_fn next, rtty_tx_echo_fn echo, void *ctx) {
    memset(t, 0, sizeof(*t));
    rtty_afsk_init(&t->afsk, cfg->rate, cfg->baud, cfg->mark_hz, cfg->space_hz);
    rtty_enc_init(&t->enc);
    t->stage = STAGE_START;
    t->baud = cfg->baud;
    t->next = next;
    t->echo = echo;
    t->ctx = ctx;
}

/* The next tones into the queue, which is empty now; false at the end. A character at a time, asked for only once
 * the one before is made into samples: one queued while the line goes out is sent with it */
static bool more(rtty_tx_t *t) {
    uint8_t codes[3];
    int     c, k;

    switch (t->stage) {
        case STAGE_START:
            rtty_afsk_tone(&t->afsk, true, RTTY_TX_IDLE_S * t->baud);
            for (int i = 0; i < RTTY_TX_DIDDLES; i++) {
                rtty_afsk_code(&t->afsk, RTTY_ITA2_LTRS);
            }
            rtty_afsk_code(&t->afsk, RTTY_ITA2_CR);
            rtty_afsk_code(&t->afsk, RTTY_ITA2_LF);
            t->stage = STAGE_TEXT;
            return true;

        case STAGE_TEXT:
            while ((c = t->next ? t->next(t->ctx) : -1) >= 0) {
                k = rtty_enc_char(&t->enc, (char)c, codes);
                if (k == 0) continue;
                for (int i = 0; i < k; i++) {
                    rtty_afsk_code(&t->afsk, codes[i]);
                }
                if (t->echo) t->echo(rtty_enc_shown((char)c), t->ctx);
                return true;
            }
            rtty_afsk_tone(&t->afsk, true, RTTY_TX_TAIL_BITS);
            rtty_afsk_fade(&t->afsk);
            t->stage = STAGE_DONE;
            return true;

        default:
            return false;
    }
}

size_t rtty_tx_fill(int16_t *buf, size_t n, void *arg) {
    rtty_tx_t *t = arg;
    size_t     out = 0;

    while (out < n) {
        if (!rtty_afsk_busy(&t->afsk) && !more(t)) break;
        out += rtty_afsk_render(&t->afsk, buf + out, n - out);
    }
    return out;
}
