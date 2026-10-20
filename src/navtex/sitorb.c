/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - NAVTEX (SITOR-B, CCIR 476 FEC) receiver, see sitorb.h
 *
 *  Chain, per audio sample (11025 Hz):
 *    - two quadrature mixers bring mark (1085 Hz) and space (915 Hz) to 0 Hz;
 *    - a raised-cosine low pass, done as a 512-point block transform on 256-sample
 *      blocks (overlap-add, only the 13 bins around 0 Hz are non-zero, so they are
 *      computed directly instead of with a full FFT);
 *    - envelope and noise floor tracked on each tone, then the mark/space difference
 *      with automatic threshold, the sign gives the bit and the size its weight;
 *    - bit timing: the weight is averaged at three points of the bit (early, prompt
 *      and late, 1/5 of a bit apart), every 8 bits the sampling instant moves towards
 *      the side with more energy;
 *    - framing: 100 weighted bits are kept, 7-bit characters with 4 ones are searched
 *      for the DX/RX interleave (each character sent twice, 5 characters apart), then
 *      read in the RX phase with the DX copy as a spare.
 */

#include "sitorb.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define RATE            11025
#define BAUD            100.0
#define TONE_MARK       1085.0          /* 1000 Hz centre, +-85 Hz */
#define TONE_SPACE      915.0

#define BLOCK           256             /* new samples per block transform */
#define NFFT            (2 * BLOCK)     /* zero padded transform length */
#define NBINS           6               /* bins -6..+6 pass the low pass, all others are 0 */

#define NBITS           100             /* weighted bits kept for framing (one second) */
#define FEC_GAP         35              /* the DX copy comes 5 characters (35 bits) before the RX one */

/* CCIR 476 control codes */
#define C_LTRS          0x5a
#define C_FIGS          0x36
#define C_ALPHA         0x0f
#define C_BETA          0x33
#define C_CHAR32        0x6a
#define C_REP           0x66

typedef struct {
    double re, im;
} cpx_t;

/* Low pass of one tone: collects BLOCK samples, outputs BLOCK filtered samples */
typedef struct {
    cpx_t   in[BLOCK];
    cpx_t   tail[BLOCK];                /* second half of the previous block's output */
    cpx_t   out[BLOCK];
    int     fill;
} tone_lpf_t;

typedef enum {
    HUNT_RESET,                         /* clear the error count and the shift, then hunt */
    HUNT,                               /* looking for the DX/RX interleave */
    READ                                /* reading characters */
} rx_state_t;

struct navtex_decoder {
    bool        reverse;

    double      mark_phase, space_phase;
    tone_lpf_t  mark_lpf, space_lpf;

    double      mark_env, space_env;
    double      mark_noise, space_noise;

    double      n;                      /* filtered samples so far (a double counts exactly for 25000 years) */
    int         tick;                   /* n modulo 8 bits, for adjust_timing() */
    double      spb;                    /* samples per bit: 110.25 */
    double      t_early, t_prompt, t_late;
    double      e_early, e_prompt, e_late;

    rx_state_t  state;
    int         bits[NBITS];
    int         cursor;                 /* start of the next character in bits[] */
    bool        rx_phase;               /* the character at cursor is the RX copy */
    int         errors;
    bool        figs;
    int         last_code;

    navtex_char_cb_t char_cb;
    void            *char_ud;
    void           (*bit_cb)(int mark, void *userdata);
    void            *bit_ud;
};

/* ---- low pass ------------------------------------------------------------ */

static cpx_t   cos_sin[NFFT];           /* cos and sin of 2 pi m / NFFT */
static cpx_t   lpf_gain[2 * NBINS + 1]; /* bins -NBINS..+NBINS */
static bool    tables_ready;

/*
 * Raised cosine (cos^2) shaped on the bit rate with a 1.4 widening, divided by
 * sinc to flatten the Nyquist channel, and a delay of NFFT / 4 samples.
 */
static void make_tables(void) {
    double width = BAUD / RATE * 1.4;

    for (int m = 0; m < NFFT; m++) {
        cos_sin[m].re = cos(2.0 * M_PI * m / NFFT);
        cos_sin[m].im = sin(2.0 * M_PI * m / NFFT);
    }
    for (int k = 0; k <= NBINS; k++) {
        double x = (double)k / BLOCK;
        double g = x <= 0 ? 1.0 : x > 2.0 * width ? 0.0 : cos(M_PI * x / (width * 4.0));

        g *= g;
        if (k) {
            double x2 = 2.0 * k * width;
            g /= sin(M_PI * x2) / (M_PI * x2);
        }
        /* delay: -k pi/2 on bin k, +k pi/2 on bin -k */
        double a = (double)k * -0.5 * M_PI, b = (double)k * 0.5 * M_PI;

        /* 1 / NFFT of the inverse transform folded in (a power of 2: exact) */
        lpf_gain[NBINS + k] = (cpx_t){g * cos(a) / NFFT, g * sin(a) / NFFT};
        lpf_gain[NBINS - k] = (cpx_t){g * cos(b) / NFFT, g * sin(b) / NFFT};
    }
    tables_ready = true;
}

static inline cpx_t cmul(cpx_t a, cpx_t b) {
    return (cpx_t){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

/* Adds one sample; returns true when out[] holds BLOCK new filtered samples */
static bool lpf_push(tone_lpf_t *f, cpx_t v) {
    f->in[f->fill++] = v;
    if (f->fill < BLOCK)
        return false;
    f->fill = 0;

    /*
     * Spectrum of the zero padded block on the pass bins, times the filter. Bins k and -k
     * share the cosine and sine sums: X(+-k) = C -+ jS.
     */
    cpx_t spec[2 * NBINS + 1];
    cpx_t c0 = {0, 0};

    for (int i = 0; i < BLOCK; i++) {
        c0.re += f->in[i].re;
        c0.im += f->in[i].im;
    }
    spec[NBINS] = cmul(c0, lpf_gain[NBINS]);

    for (int k = 1; k <= NBINS; k++) {
        cpx_t c = {0, 0}, s = {0, 0};

        for (int i = 0; i < BLOCK; i++) {
            cpx_t w = cos_sin[(k * i) & (NFFT - 1)];

            c.re += f->in[i].re * w.re;
            c.im += f->in[i].im * w.re;
            s.re += f->in[i].re * w.im;
            s.im += f->in[i].im * w.im;
        }
        spec[NBINS + k] = cmul((cpx_t){c.re + s.im, c.im - s.re}, lpf_gain[NBINS + k]);
        spec[NBINS - k] = cmul((cpx_t){c.re - s.im, c.im + s.re}, lpf_gain[NBINS - k]);
    }

    /* back to time, NFFT points: the first half plus the previous tail is the output */
    for (int i = 0; i < NFFT; i++) {
        double re = spec[NBINS].re, im = spec[NBINS].im;

        for (int k = 1; k <= NBINS; k++) {
            cpx_t w = cos_sin[(k * i) & (NFFT - 1)];
            cpx_t p = spec[NBINS + k], m = spec[NBINS - k];

            /* Z(k) e^(j th) + Z(-k) e^(-j th) */
            re += (p.re + m.re) * w.re - (p.im - m.im) * w.im;
            im += (p.im + m.im) * w.re + (p.re - m.re) * w.im;
        }
        if (i < BLOCK) {
            f->out[i].re = f->tail[i].re + re;
            f->out[i].im = f->tail[i].im + im;
        } else {
            f->tail[i - BLOCK] = (cpx_t){re, im};
        }
    }
    return true;
}

/* ---- CCIR 476 ------------------------------------------------------------ */

/* The 29 printing codes: letters and figures case */
static const struct {
    unsigned char code;
    char          ltr, fig;
} ccir476[] = {
    {0x17, 'J', '\''}, {0x1b, 'F', '!'}, {0x1d, 'C', ':'}, {0x1e, 'K', '('},
    {0x27, 'W', '2'},  {0x2b, 'Y', '6'}, {0x2d, 'P', '0'}, {0x2e, 'Q', '1'},
    {0x35, 'G', '&'},  {0x39, 'M', '.'}, {0x3a, 'X', '/'}, {0x3c, 'V', ';'},
    {0x47, 'A', '-'},  {0x4b, 'S', 7},   {0x4d, 'I', '8'}, {0x4e, 'U', '7'},
    {0x53, 'D', '$'},  {0x55, 'R', '4'}, {0x56, 'E', '3'}, {0x59, 'N', ','},
    {0x5c, ' ', ' '},  {0x63, 'Z', '"'}, {0x65, 'L', ')'}, {0x69, 'H', '#'},
    {0x6c, '\n', '\n'}, {0x71, 'O', '9'}, {0x72, 'B', '?'}, {0x74, 'T', '5'},
    {0x78, '\r', '\r'},
};

static int ccir_char(int code, bool figs) {
    for (size_t i = 0; i < sizeof(ccir476) / sizeof(ccir476[0]); i++)
        if (ccir476[i].code == code)
            return figs ? ccir476[i].fig : ccir476[i].ltr;
    return -1;
}

/* Valid CCIR 476 characters have exactly 4 of 7 bits set */
static bool code_ok(int code) {
    return __builtin_popcount((unsigned)code) == 4;
}

/* 7 weighted bits, LSB first, to a code: positive weight = 1 */
static int code_at(const int *w) {
    int code = 0;

    for (int i = 0; i < 7; i++)
        if (w[i] > 0)
            code |= 1 << i;
    return code;
}

/*
 * Repair try for a character with one bit too few or too many: flip the weakest bit
 * of the side that has one extra. Zero weights count as ones here (and as zeros in
 * code_at), and the "too many ones" branch is reached only in that corner: kept as
 * the receiver always worked.
 */
static void flip_weakest(int *w) {
    int neg = 0, pos = 1, neg_at = -1, pos_at = -1;
    int neg_max = INT_MIN, pos_min = INT_MAX;

    for (int i = 0; i < 7; i++) {
        if (w[i] < 0) {
            neg++;
            if (w[i] > neg_max) {
                neg_max = w[i];
                neg_at = i;
            }
        } else {
            pos++;
            if (w[i] < pos_min) {
                pos_min = w[i];
                pos_at = i;
            }
        }
    }
    if (neg == 4)
        w[neg_at] = -w[neg_at];
    else if (pos == 5)
        w[pos_at] = -w[pos_at];
}

/* ---- framing ------------------------------------------------------------- */

static void emit(navtex_decoder_t *d, int c) {
    if (c == 7)
        c = '\'';           /* BELL: French stations use it as an apostrophe */
    else if (c == '\r')
        return;
    if (d->char_cb)
        d->char_cb(c, d->char_ud);
}

static void take_code(navtex_decoder_t *d, int code) {
    switch (code) {
        case C_REP:
            /* two REPs in a row in the RX phase: the phase is wrong, swap it */
            if (d->last_code == C_REP)
                d->rx_phase = false;
            break;
        case C_ALPHA:
        case C_BETA:
        case C_CHAR32:
            break;
        case C_LTRS:
            d->figs = false;
            break;
        case C_FIGS:
            d->figs = true;
            break;
        default: {
            int c = ccir_char(code, d->figs);

            if (c >= 0)
                emit(d, c);
            break;
        }
    }
    d->last_code = code;
}

/*
 * Reads the RX copy at pos, falling back to the DX copy 35 bits earlier.
 * Returns 1 when the RX copy was good, 0 when the DX copy was taken as is,
 * -1 when the character had to be rebuilt, -2 when nothing worked.
 */
static int read_char(navtex_decoder_t *d, int pos) {
    int *rx = &d->bits[pos];
    int  code = code_at(rx);

    if (code_ok(code)) {
        take_code(d, code);
        return 1;
    }
    if (pos < FEC_GAP)
        return -1;

    int *dx = &d->bits[pos - FEC_GAP];
    int  spare = code_at(dx);

    if (code_ok(spare)) {
        if (spare == C_REP)     /* phasing, nothing to print and no phase swap */
            return 0;
        take_code(d, spare);
        return 0;
    }

    /* both broken: the sum of the two copies, then single bit flips */
    int sum[7];

    for (int i = 0; i < 7; i++) {
        long long s = (long long)rx[i] + dx[i];

        sum[i] = s > INT_MAX ? INT_MAX : s < -INT_MAX ? -INT_MAX : (int)s;
    }

    int *tries[] = {sum, rx, dx, sum};

    for (int t = 0; t < 4; t++) {
        if (t)
            flip_weakest(tries[t]);
        code = code_at(tries[t]);
        if (code_ok(code)) {
            take_code(d, code);
            return -1;
        }
    }
    return -2;
}

/*
 * Best start of a character in bits[]: the offset (14 choices, DX/RX interleave)
 * with the most valid characters, at least 3 of them matching their DX copy or
 * an ALPHA right after a REP. -1 when no offset scores more than 8.
 */
static int find_phase(const navtex_decoder_t *d) {
    int best = -1, best_score = 0;

    for (int off = FEC_GAP; off < FEC_GAP + 14; off++) {
        int score = 0, pairs = 0;

        for (int i = off; i < NBITS - 7; i += 7) {
            int ones = 0;

            for (int b = 0; b < 7; b++)
                ones += d->bits[i + b] > 0;
            if (ones != 4)
                continue;

            int code = code_at(&d->bits[i]);
            int dx = code_at(&d->bits[i - FEC_GAP]);

            score++;
            if (code == dx) {
                if (code == C_ALPHA || code == C_REP) {
                    /* phasing codes equal 5 characters apart: wrong parity */
                    score = 0;
                    continue;
                }
                pairs++;
            } else if (code == C_ALPHA && code_at(&d->bits[i - 7]) == C_REP) {
                pairs++;
            }
        }
        if (pairs >= 3 && score + pairs > best_score) {
            best_score = score + pairs;
            best = off;
        }
    }
    return best_score > 8 ? best : -1;
}

static void take_bit(navtex_decoder_t *d, int weight) {
    memmove(d->bits, d->bits + 1, (NBITS - 1) * sizeof(int));
    d->bits[NBITS - 1] = weight;
    if (d->cursor > 0)
        d->cursor--;

    if (d->state == HUNT) {
        int off = find_phase(d);

        if (off >= 0) {
            d->state = READ;
            d->cursor = off;
            d->rx_phase = true;
        } else {
            d->state = HUNT_RESET;
        }
    }

    if (d->state != READ || d->cursor >= NBITS - 7)
        return;

    if (d->rx_phase) {
        d->errors -= read_char(d, d->cursor);
        if (d->errors > 5)
            d->figs = false;
        if (d->errors > 12)
            d->state = HUNT_RESET;
        if (d->errors < 0)
            d->errors = 0;
    }
    d->rx_phase = !d->rx_phase;
    d->cursor += 7;
}

/* ---- demodulator --------------------------------------------------------- */

static inline double smooth(double avg, double v, int weight) {
    return weight <= 1 ? v : (v - avg) / weight + avg;
}

/* envelope: up fast, down slow; noise floor: down fast, up slow */
static inline double track_env(const navtex_decoder_t *d, double avg, double v) {
    return smooth(avg, v, v > avg ? (int)(d->spb / 4) : (int)(d->spb * 16));
}

static inline double track_noise(const navtex_decoder_t *d, double avg, double v) {
    return smooth(avg, v, v < avg ? (int)(d->spb / 4) : (int)(d->spb * 48));
}

/* Every 8 bits: move the sampling instants towards the stronger side */
static void adjust_timing(navtex_decoder_t *d) {
    bool now = d->tick == 0;

    if (++d->tick >= (int)(d->spb * 8))
        d->tick = 0;
    if (!now)
        return;

    double step = (d->e_late - d->e_early) / (d->e_prompt + 0.1) * 22;

    if (d->e_prompt > d->e_early && d->e_prompt > d->e_late)
        step /= 2;
    d->t_early += step;
    d->t_prompt += step;
    d->t_late += step;
}

static int clamp_int(double v) {
    if (v >= INT_MAX)
        return INT_MAX;
    if (v <= -INT_MAX)
        return -INT_MAX;
    return (int)v;
}

static void demod(navtex_decoder_t *d, const cpx_t *mark, const cpx_t *space) {
    for (int i = 0; i < BLOCK; i++) {
        double m = hypot(mark[i].re, mark[i].im);
        double s = hypot(space[i].re, space[i].im);

        adjust_timing(d);

        d->mark_env = track_env(d, d->mark_env, m);
        d->mark_noise = track_noise(d, d->mark_noise, m);
        d->space_env = track_env(d, d->space_env, s);
        d->space_noise = track_noise(d, d->space_noise, s);

        double floor = (d->space_noise + d->mark_noise) / 2;

        m = fmax(fmin(m, d->mark_env), floor);
        s = fmax(fmin(s, d->space_env), floor);

        /* mark minus space, each weighted by its own envelope (automatic threshold) */
        double me = d->mark_env - floor, se = d->space_env - floor;
        double level = (m - floor) * me - (s - floor) * se - 0.5 * (me * me - se * se);
        double mag = fabs(level);

        if (d->n >= d->t_early) {
            d->e_early = smooth(d->e_early, mag, 64);
            d->t_early += d->spb;
        }
        if (d->n >= d->t_late) {
            d->e_late = smooth(d->e_late, mag, 64);
            d->t_late += d->spb;
        }
        bool bit_now = d->n >= d->t_prompt;

        if (bit_now) {
            d->e_prompt = smooth(d->e_prompt, mag, 64);
            d->t_prompt += d->spb;
        }

        if (d->state == HUNT_RESET) {
            d->errors = 0;
            d->figs = false;
            d->state = HUNT;
        } else if (bit_now) {
            if (d->bit_cb)
                d->bit_cb(level > 0, d->bit_ud);
            take_bit(d, clamp_int(level));
        }
        d->n++;
    }
}

/* Mixes a sample down by the tone; the real sample goes on both I and Q */
static inline cpx_t mix(double *phase, double tone, double v) {
    double c = cos(*phase), s = sin(*phase);

    *phase -= 2.0 * M_PI * tone / RATE;
    if (*phase < -2.0 * M_PI)
        *phase += 2.0 * M_PI;
    return (cpx_t){c * v - s * v, c * v + s * v};
}

/* ---- API ----------------------------------------------------------------- */

navtex_decoder_t *navtex_decoder_create(int reverse) {
    if (!tables_ready)
        make_tables();

    navtex_decoder_t *d = calloc(1, sizeof(*d));

    if (!d)
        return NULL;
    d->reverse = reverse != 0;
    d->spb = RATE / BAUD;
    d->t_early = 0;
    d->t_prompt = d->spb / 5;
    d->t_late = d->spb * 2 / 5;
    d->state = HUNT_RESET;
    return d;
}

void navtex_decoder_destroy(navtex_decoder_t *d) {
    free(d);
}

void navtex_decoder_set_char_callback(navtex_decoder_t *d, navtex_char_cb_t cb, void *userdata) {
    if (d) {
        d->char_cb = cb;
        d->char_ud = userdata;
    }
}

void navtex_decoder_set_bit_callback(navtex_decoder_t *d, void (*cb)(int mark, void *userdata), void *userdata) {
    if (d) {
        d->bit_cb = cb;
        d->bit_ud = userdata;
    }
}

void navtex_decoder_process(navtex_decoder_t *d, const float *samples, unsigned int n) {
    if (!d || !samples)
        return;

    for (unsigned int i = 0; i < n; i++) {
        double v = 32767.0f * samples[i];   /* the int16 scale the levels below are tuned on */
        cpx_t  mk = mix(&d->mark_phase, TONE_MARK, v);
        cpx_t  sp = mix(&d->space_phase, TONE_SPACE, v);

        bool done = lpf_push(&d->mark_lpf, mk);

        lpf_push(&d->space_lpf, sp);
        if (!done)
            continue;
        if (d->reverse)
            demod(d, d->space_lpf.out, d->mark_lpf.out);
        else
            demod(d, d->mark_lpf.out, d->space_lpf.out);
    }
}
