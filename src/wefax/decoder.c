// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - WeFax receiver, see decoder.h
//
// The Auto logic follows the approach of 1KO125's x6100_gui 1.0.2 (Hany El
// Imam): the APT tones are measured on the demodulated grey signal, a start
// only counts once the black band after it shows up, and the line is
// aligned on the white pulse of the phasing lines.

#include "decoder.h"     /* wefax_decoder_t, WEFAX_* */

#include <math.h>        /* sinf, cosf */
#include <stddef.h>
#include <stdio.h>       /* printf, the Auto log lines */
#include <stdlib.h>      /* calloc, free */
#include <string.h>

#define PI_F            3.14159265358979323846f
#define TWO_PI_F        (2.0f * PI_F)

/* Front end: mix the centre to 0 Hz, low-pass, phase difference of two samples */
#define NCO_STEP        (2.0f * PI_F * (float)WEFAX_CENTER_HZ / (float)WEFAX_SAMPLE_RATE)
#define LPF_HZ          600.0f
#define LPF_TAPS        63
#define GREY_PER_RAD    (((float)WEFAX_SAMPLE_RATE / (float)WEFAX_SHIFT_HZ) / (2.0f * PI_F))

/* Share of a line per sample */
#define LINE_STEP       ((double)WEFAX_LPM / (60.0 * (double)WEFAX_SAMPLE_RATE))

/*
 * APT tones (black/white square wave on the grey signal: 300 Hz start, 450
 * Hz stop), measured on half second windows in two ways:
 *  - rising edges through fixed levels (start) or levels taken from the
 *    window itself, 5th..95th percentile (stop: still works with the radio
 *    mistuned by 100 Hz and more; a window with less swing is no tone);
 *  - the share of the grey power in the three 2 Hz bins around the tone:
 *    survives far more noise than the edges. Real picture rows stay below
 *    0.03 at 300 Hz and 0.017 at 450 Hz, the tones give 0.3..0.6.
 */
#define TONE_WINDOW     (WEFAX_SAMPLE_RATE / 2)
#define TONE_WHITE      215
#define TONE_BLACK      40
#define START_HZ        300.0f
#define START_TOL_HZ    30.0f
#define START_SHARE     0.12f
#define START_WINDOWS   2
#define STOP_HZ         450.0f
#define STOP_TOL_HZ     30.0f
#define STOP_SHARE      0.10f
#define STOP_MIN_SWING  150

/*
 * A start is real when 4 rows in a row are black (85% of the pixels below
 * 80: a phasing row is black but for its 5% pulse). Measured on saved faxes:
 * this passes 34 of 39 phasing rows on the worst one, no picture or noise
 * row. The rows must come within 10 s of the last start window (4.3..6.5 s
 * seen on real faxes; longer lets a stray 300 Hz hit reach some other black
 * band).
 */
#define BLACK_LEVEL     80
#define BLACK_PERCENT   85
#define BLACK_ROWS      4
#define START_TIMEOUT   (WEFAX_SAMPLE_RATE * 10)

/*
 * After a stop the app saves and re-arms while the stop tone is still on
 * the air. A worn stop tone can read as 300 Hz and the 10 s of black after
 * it would confirm a fax that is not there: no start for this long.
 */
#define STOP_HOLDOFF    (WEFAX_SAMPLE_RATE * 8)

/*
 * Phasing (Auto, after the confirmation): black rows with a white pulse at
 * the line start. Columns of 4 black rows are averaged and the pulse is
 * looked for, circularly. When 5 of the last 8 groups agree within 8 px the
 * line is moved once, so that each row starts right after the pulse (the
 * white bar ends up on the right border: right for GYA, DWD charts are
 * narrower than the line anyway). If it is not clear nothing moves.
 * Measured on 3 saved faxes and 2 recordings: pulse 84..90 px wide,
 * contrast 180..220 (noise <= 53, picture <= 83), the picture starts with
 * bright rows (mean >= 141), which ends the search.
 */
#define PH_GROUP        4
#define PH_WINDOW       64
#define PH_CONTRAST     100
#define PH_WIDTH_MIN    30
#define PH_WIDTH_MAX    130
#define PH_AGREE        8
#define PH_VOTES        5
#define PH_KEEP         8
#define PH_ROWS_MAX     40
#define PH_BRIGHT_ROWS  3
#define PH_BRIGHT_MEAN  135
#define PH_EDGE_SPAN    150

struct wefax_decoder {                      /* opaque in decoder.h */
    wefax_mode_t            mode;           /* IDLE, CONT or AUTO */
    wefax_row_callback_t    row_cb;
    void                   *row_cb_data;
    float                   taps[LPF_TAPS];

    /* everything below is cleared by a reset */
    float       nco;
    float       hist_i[2 * LPF_TAPS];       /* each sample twice: no wrap in the filter */
    float       hist_q[2 * LPF_TAPS];
    unsigned    hist_pos;
    float       last_i, last_q;
    bool        primed;

    /* tone meter */
    bool        tone_level_known, tone_white;
    unsigned    tone_n, tone_rises;
    uint8_t     tone_buf[TONE_WINDOW];

    /* Auto */
    bool        started, confirmed, stopped;
    unsigned    start_hits, black_rows;
    uint32_t    start_age, generation, holdoff;
    bool        next_seen;                  /* start tone over a confirmed fax, waiting for its black band */
    unsigned    next_hits, next_black_rows;
    uint32_t    next_age;

    /* line */
    double      line_pos;
    unsigned    px, px_n;
    double      px_sum;
    uint32_t    rows;
    uint8_t     row[WEFAX_WIDTH];       /* the line being built */

    /* phasing */
    bool        ph_on, ph_skip;
    unsigned    ph_rows, ph_black_run, ph_bright_run, ph_found;
    uint8_t     ph_ring[PH_GROUP][WEFAX_WIDTH];
    int         ph_left[PH_KEEP], ph_width[PH_KEEP];
};

static void make_taps(float *taps)
{
    const float fc  = LPF_HZ / (float)WEFAX_SAMPLE_RATE;
    float       sum = 0.0f;

    /* windowed sinc (Hamming), unity gain at DC */
    for (int n = 0; n < LPF_TAPS; n++) {
        int   k = n - (LPF_TAPS - 1) / 2;
        float s = k ? sinf(2.0f * PI_F * fc * (float)k) / (PI_F * (float)k) : 2.0f * fc;
        float w = 0.54f - 0.46f * cosf(2.0f * PI_F * (float)n / (float)(LPF_TAPS - 1));

        taps[n] = s * w;
        sum += taps[n];
    }
    for (int n = 0; n < LPF_TAPS; n++)
        taps[n] /= sum;
}

static void clear_state(wefax_decoder_t *d)
{
    size_t keep = offsetof(struct wefax_decoder, nco);

    memset((char *)d + keep, 0, sizeof(*d) - keep);
}

wefax_decoder_t *wefax_decoder_create(void)
{
    wefax_decoder_t *d = calloc(1, sizeof(*d));

    if (d)
        make_taps(d->taps);
    return d;
}

void wefax_decoder_destroy(wefax_decoder_t *decoder)
{
    free(decoder);
}

void wefax_decoder_reset(wefax_decoder_t *d)
{
    if (!d)
        return;

    bool after_stop = d->mode == WEFAX_MODE_AUTO && d->stopped;

    clear_state(d);
    if (after_stop) {
        d->holdoff = STOP_HOLDOFF;
        printf("WEFAX AUTO: no start for %u s after the stop\n", STOP_HOLDOFF / WEFAX_SAMPLE_RATE);
    }
}

void wefax_decoder_set_mode(wefax_decoder_t *d, wefax_mode_t mode)
{
    if (!d)
        return;
    d->mode = mode;
    wefax_decoder_reset(d);
}

wefax_mode_t wefax_decoder_get_mode(const wefax_decoder_t *d)
{
    return d ? d->mode : WEFAX_MODE_IDLE;
}

void wefax_decoder_set_row_callback(wefax_decoder_t *d, wefax_row_callback_t callback, void *user_data)
{
    if (!d)
        return;
    d->row_cb      = callback;
    d->row_cb_data = user_data;
}

bool wefax_decoder_auto_stop_detected(const wefax_decoder_t *d)
{
    return d && d->stopped;
}

uint32_t wefax_decoder_auto_start_generation(const wefax_decoder_t *d)
{
    return d ? d->generation : 0;
}

bool wefax_decoder_auto_fax_confirmed(const wefax_decoder_t *d)
{
    return d && d->confirmed;
}

/* ---- tone measurements on a window of grey samples ---- */

/* Rising edges per second, levels from the window's own 5th/95th percentile; 0 without enough swing */
static float tone_rate_own_levels(const uint8_t *buf, unsigned n)
{
    unsigned count[256] = {0};
    unsigned lo_at = (n * 5U + 99U) / 100U, hi_at = (n * 95U + 99U) / 100U, acc = 0;
    int      lo = -1, hi = 255;

    for (unsigned i = 0; i < n; i++)
        count[buf[i]]++;
    for (int v = 0; v < 256; v++) {
        acc += count[v];
        if (lo < 0 && acc >= lo_at)
            lo = v;
        if (acc >= hi_at) {
            hi = v;
            break;
        }
    }

    int swing = hi - lo;

    if (lo < 0 || swing < STOP_MIN_SWING)
        return 0.0f;

    float    mid   = 0.5f * (float)(lo + hi);
    float    up    = mid + 0.25f * (float)swing;
    float    down  = mid - 0.25f * (float)swing;
    bool     high  = (float)buf[0] > mid;
    unsigned rises = 0;

    for (unsigned i = 0; i < n; i++) {
        float v = (float)buf[i];

        if (v > up && !high) {
            high = true;
            rises++;
        } else if (v < down && high) {
            high = false;
        }
    }
    return (float)WEFAX_SAMPLE_RATE * (float)rises / (float)n;
}

/* Share (0..1) of the window's grey power in the 3 DFT bins nearest to hz (Goertzel) */
static float tone_share(const uint8_t *buf, unsigned n, float hz)
{
    double mean = 0.0, power = 0.0, line = 0.0;

    for (unsigned i = 0; i < n; i++)
        mean += buf[i];
    mean /= (double)n;
    for (unsigned i = 0; i < n; i++) {
        double v = (double)buf[i] - mean;

        power += v * v;
    }
    if (power < 1.0)
        return 0.0f;

    int bin = (int)(hz / ((double)WEFAX_SAMPLE_RATE / (double)n) + 0.5);

    for (int k = bin - 1; k <= bin + 1; k++) {
        double c  = 2.0 * cos(2.0 * 3.14159265358979323846 * (double)k / (double)n);
        double s1 = 0.0, s2 = 0.0;

        for (unsigned i = 0; i < n; i++) {
            double s0 = ((double)buf[i] - mean) + c * s1 - s2;

            s2 = s1;
            s1 = s0;
        }
        line += s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    return (float)(2.0 * line / ((double)n * power));      /* one-sided: total n * power / 2 */
}

/* ---- phasing ---- */

static void phasing_begin(wefax_decoder_t *d)
{
    d->ph_on         = true;
    d->ph_skip       = false;
    d->ph_rows       = 0;
    d->ph_black_run  = 0;
    d->ph_bright_run = 0;
    d->ph_found      = 0;
}

/* a - b around the line, in (-W/2, W/2] */
static int wrap_diff(int a, int b)
{
    int d = (a - b) % WEFAX_WIDTH;

    if (d > WEFAX_WIDTH / 2)
        d -= WEFAX_WIDTH;
    else if (d <= -WEFAX_WIDTH / 2)
        d += WEFAX_WIDTH;
    return d;
}

static float column_mean(const float *mean, int x)
{
    return mean[(x + 2 * WEFAX_WIDTH) % WEFAX_WIDTH];
}

/* The pulse in the column means of the last PH_GROUP black rows: left edge and width */
static bool pulse_find(const wefax_decoder_t *d, int *left, int *width)
{
    static float mean[WEFAX_WIDTH];
    float        total = 0.0f;

    for (unsigned x = 0; x < WEFAX_WIDTH; x++) {
        unsigned sum = 0;

        for (unsigned r = 0; r < PH_GROUP; r++)
            sum += d->ph_ring[r][x];
        mean[x] = (float)sum / (float)PH_GROUP;
        total += mean[x];
    }

    /* brightest PH_WINDOW columns */
    float    win = 0.0f, best;
    unsigned at  = 0;

    for (unsigned x = 0; x < PH_WINDOW; x++)
        win += mean[x];
    best = win;
    for (unsigned x = 1; x < WEFAX_WIDTH; x++) {
        win += mean[(x + PH_WINDOW - 1) % WEFAX_WIDTH] - mean[x - 1];
        if (win > best) {
            best = win;
            at   = x;
        }
    }

    float rest     = (total - best) / (float)(WEFAX_WIDTH - PH_WINDOW);
    float contrast = best / (float)PH_WINDOW - rest;

    if (contrast < (float)PH_CONTRAST)
        return false;

    float half   = rest + contrast / 2.0f;
    int   centre = (int)((at + PH_WINDOW / 2) % WEFAX_WIDTH);

    if (mean[centre] <= half)
        return false;

    /* edges: from outside inwards, the first 3 columns above half (dips inside the pulse don't cut it) */
    int l, r;

    for (l = -PH_EDGE_SPAN; l <= 0; l++)
        if (column_mean(mean, centre + l) > half && column_mean(mean, centre + l + 1) > half &&
            column_mean(mean, centre + l + 2) > half)
            break;
    for (r = PH_EDGE_SPAN; r >= 0; r--)
        if (column_mean(mean, centre + r) > half && column_mean(mean, centre + r - 1) > half &&
            column_mean(mean, centre + r - 2) > half)
            break;
    if (l > 0 || r < 0)
        return false;

    *width = r - l + 1;
    *left  = (centre + l + 2 * WEFAX_WIDTH) % WEFAX_WIDTH;
    return *width >= PH_WIDTH_MIN && *width <= PH_WIDTH_MAX;
}

static void sort_ints(int *v, unsigned n)
{
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && v[j - 1] > v[j]; j--) {
            int t = v[j];

            v[j]     = v[j - 1];
            v[j - 1] = t;
        }
}

static bool row_is_black(const uint8_t *row)
{
    unsigned dark = 0;

    for (unsigned x = 0; x < WEFAX_WIDTH; x++)
        dark += row[x] < BLACK_LEVEL;
    return dark * 100U >= WEFAX_WIDTH * BLACK_PERCENT;
}

/* One finished row while phasing: the column where the next line must start, or -1 */
static int phasing_row(wefax_decoder_t *d, const uint8_t *row)
{
    if (d->ph_skip) {
        d->ph_skip = false;
        return -1;
    }
    d->ph_rows++;

    bool     black = row_is_black(row);
    unsigned sum   = 0;

    for (unsigned x = 0; x < WEFAX_WIDTH; x++)
        sum += row[x];
    d->ph_bright_run = sum >= (unsigned)PH_BRIGHT_MEAN * WEFAX_WIDTH ? d->ph_bright_run + 1 : 0;

    if (!black) {
        d->ph_black_run = 0;
    } else {
        memcpy(d->ph_ring[d->ph_black_run++ % PH_GROUP], row, WEFAX_WIDTH);

        int left, width;

        if (d->ph_black_run >= PH_GROUP && pulse_find(d, &left, &width)) {
            d->ph_left[d->ph_found % PH_KEEP]  = left;
            d->ph_width[d->ph_found % PH_KEEP] = width;
            d->ph_found++;

            /* the estimate with most others within PH_AGREE */
            unsigned n     = d->ph_found < PH_KEEP ? d->ph_found : PH_KEEP;
            unsigned votes = 0;
            int      ref   = 0;

            for (unsigned i = 0; i < n; i++) {
                unsigned v = 0;

                for (unsigned j = 0; j < n; j++) {
                    int dx = wrap_diff(d->ph_left[j], d->ph_left[i]);

                    v += dx >= -PH_AGREE && dx <= PH_AGREE;
                }
                if (v > votes) {
                    votes = v;
                    ref   = d->ph_left[i];
                }
            }

            if (votes >= PH_VOTES) {
                /* medians of the agreeing ones */
                int      dl[PH_KEEP], w[PH_KEEP];
                unsigned m = 0;

                for (unsigned j = 0; j < n; j++) {
                    int dx = wrap_diff(d->ph_left[j], ref);

                    if (dx >= -PH_AGREE && dx <= PH_AGREE) {
                        dl[m] = dx;
                        w[m]  = d->ph_width[j];
                        m++;
                    }
                }
                sort_ints(dl, m);
                sort_ints(w, m);

                int start = (ref + dl[m / 2] + WEFAX_WIDTH) % WEFAX_WIDTH;
                int end   = (start + w[m / 2]) % WEFAX_WIDTH;     /* first column after the pulse */

                d->ph_on = false;
                printf("WEFAX AUTO: phasing pulse at %d..%d (%d px, %u of %u groups), line moved\n", start,
                       (end + WEFAX_WIDTH - 1) % WEFAX_WIDTH, w[m / 2], votes, n);
                return end;
            }
        }
    }

    /* never move the line once the picture has begun */
    if (d->ph_bright_run >= PH_BRIGHT_ROWS || d->ph_rows >= PH_ROWS_MAX) {
        d->ph_on = false;
        printf("WEFAX AUTO: phasing unclear (%u good groups), line not moved\n", d->ph_found);
    }
    return -1;
}

/* ---- Auto ---- */

static void line_restart(wefax_decoder_t *d, double pos)
{
    d->line_pos = pos;
    d->px       = 0;
    d->px_sum   = 0.0;
    d->px_n     = 0;
    d->rows     = 0;
    memset(d->row, 0, sizeof(d->row));
}

/* Half a second of grey measured */
static void tone_window(wefax_decoder_t *d)
{
    float rate  = (float)WEFAX_SAMPLE_RATE * (float)d->tone_rises / (float)d->tone_n;
    bool  start = d->holdoff == 0 && (fabsf(rate - START_HZ) <= START_TOL_HZ ||
                                      tone_share(d->tone_buf, d->tone_n, START_HZ) >= START_SHARE);

    if (!d->confirmed) {
        if (d->started) {
            if (start)
                d->start_age = 0;       /* still the same start tone */
        } else if (!start) {
            d->start_hits = 0;
        } else if (++d->start_hits >= START_WINDOWS) {
            d->started    = true;
            d->generation++;
            d->start_hits = 0;
            d->start_age  = 0;
            d->black_rows = 0;
            line_restart(d, 0.0);
            printf("WEFAX AUTO: start tone (%.1f Hz), waiting for the black band\n", rate);
        }
        return;
    }

    float stop_rate = tone_rate_own_levels(d->tone_buf, d->tone_n);

    if (fabsf(stop_rate - STOP_HZ) <= STOP_TOL_HZ || tone_share(d->tone_buf, d->tone_n, STOP_HZ) >= STOP_SHARE) {
        d->stopped = true;
        printf("WEFAX AUTO: stop tone (%.1f Hz)\n", stop_rate);
        return;
    }

    /* a new start over the current fax: it takes over only after its own black band */
    if (d->next_seen) {
        if (start)
            d->next_age = 0;
    } else if (!start) {
        d->next_hits = 0;
    } else if (++d->next_hits >= START_WINDOWS) {
        d->next_seen       = true;
        d->next_age        = 0;
        d->next_black_rows = 0;
        d->next_hits       = 0;
        printf("WEFAX AUTO: start tone over the current fax (%.1f Hz)\n", rate);
    }
}

static void auto_sample(wefax_decoder_t *d, uint8_t grey)
{
    if (d->holdoff && --d->holdoff == 0)
        printf("WEFAX AUTO: start detection back on\n");

    if (d->started && !d->confirmed && ++d->start_age >= START_TIMEOUT) {
        d->started    = false;
        d->start_hits = 0;
        d->black_rows = 0;
        d->start_age  = 0;
        printf("WEFAX AUTO: no black band after the start tone, searching again\n");
    }
    if (d->confirmed && d->next_seen && ++d->next_age >= START_TIMEOUT) {
        d->next_seen       = false;
        d->next_black_rows = 0;
        d->next_hits       = 0;
        d->next_age        = 0;
        printf("WEFAX AUTO: no black band after the new start tone\n");
    }
    if (d->stopped)
        return;

    /* rising edges through the fixed levels, with hysteresis */
    if (!d->tone_level_known) {
        if (grey > TONE_WHITE || grey < TONE_BLACK) {
            d->tone_white       = grey > TONE_WHITE;
            d->tone_level_known = true;
        }
    } else if (!d->tone_white && grey > TONE_WHITE) {
        d->tone_white = true;
        d->tone_rises++;
    } else if (d->tone_white && grey < TONE_BLACK) {
        d->tone_white = false;
    }

    d->tone_buf[d->tone_n++] = grey;
    if (d->tone_n >= TONE_WINDOW) {
        tone_window(d);
        d->tone_n     = 0;
        d->tone_rises = 0;
    }
}

/* ---- rows ---- */

static void row_done(wefax_decoder_t *d)
{
    if (d->px_n)
        d->row[d->px] = (uint8_t)(d->px_sum / d->px_n + 0.5);

    if (d->mode == WEFAX_MODE_AUTO && (d->started || d->next_seen)) {
        bool black = row_is_black(d->row);

        if (!d->confirmed && d->started) {
            d->black_rows = black ? d->black_rows + 1 : 0;
            if (d->black_rows >= BLACK_ROWS) {
                d->confirmed       = true;
                d->start_hits      = 0;
                d->next_seen       = false;
                d->next_black_rows = 0;
                printf("WEFAX AUTO: fax confirmed\n");
                phasing_begin(d);
            }
        } else if (d->confirmed && d->next_seen) {
            d->next_black_rows = black ? d->next_black_rows + 1 : 0;
            if (d->next_black_rows >= BLACK_ROWS) {
                /* 1.0: the end of the row below takes one line off */
                line_restart(d, 1.0);
                d->generation++;
                d->next_hits       = 0;
                d->next_seen       = false;
                d->next_black_rows = 0;
                printf("WEFAX AUTO: new fax confirmed, the current one ends here\n");
                phasing_begin(d);
                d->ph_skip = true;      /* this row was just blanked */
            }
        }
    }

    int cut = -1;

    if (d->mode == WEFAX_MODE_AUTO && d->confirmed && d->ph_on)
        cut = phasing_row(d, d->row);

    if (d->row_cb)
        d->row_cb(d->row, WEFAX_WIDTH, d->rows, d->row_cb_data);
    d->rows++;

    /* next line, keeping the fraction */
    d->line_pos -= 1.0;
    if (cut > 1 && cut < WEFAX_WIDTH - 1) {
        /* this row ends right after the pulse, the next ones start there */
        d->line_pos += (double)(WEFAX_WIDTH - cut) / (double)WEFAX_WIDTH;
        if (d->line_pos >= 1.0)
            d->line_pos -= 1.0;
    }
    d->px = (unsigned)(d->line_pos * (double)WEFAX_WIDTH);
    if (d->px >= WEFAX_WIDTH)
        d->px = 0;
    d->px_sum = 0.0;
    d->px_n   = 0;
    memset(d->row, 0, sizeof(d->row));
}

/* Grey sample into the line: each pixel is the mean of its samples, a skipped pixel takes the next one */
static void line_sample(wefax_decoder_t *d, uint8_t grey)
{
    d->line_pos += LINE_STEP;

    unsigned x   = (unsigned)(d->line_pos * (double)WEFAX_WIDTH);
    bool     end = x >= WEFAX_WIDTH;

    if (end)
        x = WEFAX_WIDTH - 1;

    if (x == d->px) {
        d->px_sum += grey;
        d->px_n++;
    } else {
        if (d->px_n && d->px < WEFAX_WIDTH)
            d->row[d->px] = (uint8_t)(d->px_sum / d->px_n + 0.5);
        while (d->px + 1 < x && d->px + 1 < WEFAX_WIDTH)
            d->row[++d->px] = grey;
        d->px     = x;
        d->px_sum = grey;
        d->px_n   = 1;
    }
    if (end)
        row_done(d);
}

/* ---- audio ---- */

void wefax_decoder_process(wefax_decoder_t *d, const float *samples, unsigned int count)
{
    if (!d || !samples || d->mode == WEFAX_MODE_IDLE)
        return;

    for (unsigned n = 0; n < count; n++) {
        float x = samples[n];
        float c = cosf(d->nco);
        float s = sinf(d->nco);

        d->nco += NCO_STEP;
        if (d->nco >= TWO_PI_F)
            d->nco -= TWO_PI_F;

        /* low-pass, newest sample first */
        unsigned p = d->hist_pos;

        d->hist_i[p] = d->hist_i[p + LPF_TAPS] = x * c;
        d->hist_q[p] = d->hist_q[p + LPF_TAPS] = -x * s;
        d->hist_pos  = p + 1 < LPF_TAPS ? p + 1 : 0;

        float i = 0.0f, q = 0.0f;

        for (unsigned t = 0; t < LPF_TAPS; t++) {
            i += d->taps[t] * d->hist_i[p + LPF_TAPS - t];
            q += d->taps[t] * d->hist_q[p + LPF_TAPS - t];
        }

        if (d->primed) {
            /* phase step from the last sample: 0 at the centre, +/- shift/2 at black and white */
            float re = d->last_i * i + d->last_q * q;
            float im = d->last_i * q - d->last_q * i;
            float g  = 255.0f * (0.5f + GREY_PER_RAD * atan2f(im, re));
            uint8_t grey = (uint8_t)((g < 0.0f ? 0.0f : g > 255.0f ? 255.0f : g) + 0.5f);

            if (d->mode == WEFAX_MODE_AUTO)
                auto_sample(d, grey);
            if (d->mode != WEFAX_MODE_AUTO || d->started)
                line_sample(d, grey);
        }
        d->last_i = i;
        d->last_q = q;
        d->primed = true;
    }
}
