/*
 * SSTV transmitter (see sstv_tx.h).
 *
 * One phase-continuous oscillator for the whole transmission, -2.5 dBFS.
 * Every tone and pixel ends on an ideal time line rounded to the nearest
 * sample, so rounding never piles up along a line (no slant at the far end).
 * 1500 Hz is black, 2300 Hz white.
 */
#include "sstv_tx.h"
#include "sstv_png.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define PI    3.14159265358979323846
#define LEVEL 24575.0

typedef struct {
    int16_t *pcm;
    size_t   n, room;
    unsigned rate;
    double   phase;
    double   clock;       /* ideal end of what has been sent, samples */
} tx_t;

static bool grow(tx_t *t, size_t more)
{
    if (more > SIZE_MAX / 2 - t->n) return false;
    if (t->n + more <= t->room) return true;
    size_t room = t->room ? t->room : (size_t)t->rate * 4u;
    while (room < t->n + more) room *= 2u;
    int16_t *p = realloc(t->pcm, room * sizeof(*p));
    if (!p) return false;
    t->pcm = p;
    t->room = room;
    return true;
}

/* Tone until sample `until` */
static bool tone_to(tx_t *t, double hz, long until)
{
    const double step = 2.0 * PI * hz / (double)t->rate;

    if (until < (long)t->n || !grow(t, (size_t)(until - (long)t->n))) return false;
    while ((long)t->n < until) {
        t->pcm[t->n++] = (int16_t)lrint(sin(t->phase) * LEVEL);
        t->phase += step;
        if (t->phase >= 2.0 * PI) t->phase -= 2.0 * PI;
    }
    return true;
}

static bool tone(tx_t *t, double hz, double ms)
{
    t->clock += ms * (double)t->rate / 1000.0;
    return tone_to(t, hz, (long)llround(t->clock));
}

/* count pixel values spread evenly over ms */
static bool pixels(tx_t *t, const uint8_t *v, int count, double ms)
{
    const double from = t->clock, len = ms * (double)t->rate / 1000.0;

    for (int i = 0; i < count; ++i)
        if (!tone_to(t, 1500.0 + 800.0 * (double)v[i] / 255.0,
                     (long)llround(from + len * (double)(i + 1) / (double)count)))
            return false;
    t->clock = from + len;
    return true;
}

static bool vis(tx_t *t, int code)
{
    int ones = 0;
    bool ok = tone(t, 1900, 300) && tone(t, 1200, 10) && tone(t, 1900, 300) && tone(t, 1200, 30);
    for (int b = 0; b < 7 && ok; ++b) {
        ones += (code >> b) & 1;
        ok = tone(t, (code >> b) & 1 ? 1100 : 1300, 30);
    }
    return ok && tone(t, ones & 1 ? 1100 : 1300, 30) && tone(t, 1200, 30);    /* even parity, stop bit */
}

static uint8_t byte(double v)
{
    return v < 0.0 ? 0 : v > 255.0 ? 255 : (uint8_t)lrint(v);
}

static double luma(const uint8_t *p)
{
    double r = p[0], g = p[1], b = p[2];
    return 0.299 * r + 0.587 * g + 0.114 * b;
}

/* One colour of picture row y */
static void plane(const uint8_t *rgb, int w, int y, int c, uint8_t *out)
{
    for (int x = 0; x < w; ++x) out[x] = rgb[((size_t)y * w + x) * 3 + c];
}

static bool robot8(tx_t *t, const uint8_t *rgb)
{
    uint8_t y8[160];
    for (int y = 0; y < 120; ++y) {
        for (int x = 0; x < 160; ++x) y8[x] = byte(luma(rgb + ((size_t)y * 160 + x) * 3));
        if (!tone(t, 1200, 10.0) || !pixels(t, y8, 160, 1000.0 / 15.0 - 10.0)) return false;
    }
    return true;
}

/* Robot 36: Y on every line, then one colour difference averaged over the
 * line pair: R-Y on even lines (1500 Hz separator), B-Y on odd ones (2300) */
static bool robot36(tx_t *t, const uint8_t *rgb)
{
    uint8_t y8[320], c8[320];
    for (int y = 0; y < 240; ++y) {
        const uint8_t *pair[2] = { rgb + (size_t)(y & ~1) * 320 * 3, rgb + (size_t)((y & ~1) + 1) * 320 * 3 };
        for (int x = 0; x < 320; ++x) {
            double sum = 0.0;
            y8[x] = byte(luma(rgb + ((size_t)y * 320 + x) * 3));
            for (int k = 0; k < 2; ++k) {
                const uint8_t *p = pair[k] + x * 3;
                double         l = luma(p);
                sum += y & 1 ? (p[2] - l) / 1.772 + 128.0 : (p[0] - l) / 1.402 + 128.0;
            }
            c8[x] = byte(sum * 0.5);
        }
        if (!tone(t, 1200, 9.0) || !tone(t, 1500, 3.0) || !pixels(t, y8, 320, 88.0) ||
            !tone(t, y & 1 ? 2300 : 1500, 4.5) || !tone(t, 1900, 1.5) || !pixels(t, c8, 320, 44.0))
            return false;
    }
    return true;
}

/* Robot 72: Y, R-Y and B-Y on every line */
static bool robot72(tx_t *t, const uint8_t *rgb)
{
    uint8_t y8[320], r8[320], b8[320];
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            const uint8_t *p = rgb + ((size_t)y * 320 + x) * 3;
            double         l = luma(p);
            y8[x] = byte(l);
            r8[x] = byte((p[0] - l) / 1.402 + 128.0);
            b8[x] = byte((p[2] - l) / 1.772 + 128.0);
        }
        if (!tone(t, 1200, 9.0) || !tone(t, 1500, 3.0) || !pixels(t, y8, 320, 138.0) || !tone(t, 1500, 4.5) ||
            !tone(t, 1900, 1.5) || !pixels(t, r8, 320, 69.0) || !tone(t, 2300, 4.5) || !tone(t, 1500, 1.5) ||
            !pixels(t, b8, 320, 69.0))
            return false;
    }
    return true;
}

/* Martin: sync, then G, B, R each after a 0.572-ms gap */
static bool martin(tx_t *t, const uint8_t *rgb, double scan)
{
    uint8_t c8[320];
    for (int y = 0; y < 256; ++y) {
        if (!tone(t, 1200, 4.862)) return false;
        for (int k = 0; k < 3; ++k) {
            plane(rgb, 320, y, (k + 1) % 3, c8);         /* G, B, R */
            if (!tone(t, 1500, 0.572) || !pixels(t, c8, 320, scan)) return false;
        }
        if (!tone(t, 1500, 0.572)) return false;
    }
    return true;
}

/* Scottie: one start sync, then G, B, sync, R on every line */
static bool scottie(tx_t *t, const uint8_t *rgb, double scan)
{
    uint8_t c8[320];
    if (!tone(t, 1200, 9.0)) return false;
    for (int y = 0; y < 256; ++y) {
        for (int k = 0; k < 3; ++k) {
            plane(rgb, 320, y, (k + 1) % 3, c8);
            if ((k == 2 && !tone(t, 1200, 9.0)) || !tone(t, 1500, 1.5) || !pixels(t, c8, 320, scan)) return false;
        }
    }
    return true;
}

int sstv_tx_pcm(sstv_mode_t mode, const char *png, unsigned rate, int16_t **pcm, size_t *count)
{
    const sstv_mode_info_t *mi = sstv_mode_info(mode);
    tx_t                    t = { .rate = rate };
    uint8_t                *rgb;
    bool                    ok;
    int                     rc;

    if (!png || !pcm || !count || rate < 8000u || !mi || !mi->tx_ms) return -20;
    *pcm = NULL;
    *count = 0;
    rgb = malloc((size_t)mi->width * mi->height * 3);
    if (!rgb) return -10;
    rc = sstv_load_png_scaled(png, rgb, mi->width, mi->height);
    if (rc < 0) {
        free(rgb);
        return rc;
    }

    ok = vis(&t, mi->vis);
    switch (mode) {
    case SSTV_ROBOT8:   ok = ok && robot8(&t, rgb); break;
    case SSTV_ROBOT36:  ok = ok && robot36(&t, rgb); break;
    case SSTV_ROBOT72:  ok = ok && robot72(&t, rgb); break;
    case SSTV_MARTIN1:  ok = ok && martin(&t, rgb, 146.432); break;
    case SSTV_MARTIN2:  ok = ok && martin(&t, rgb, 73.216); break;
    case SSTV_SCOTTIE1: ok = ok && scottie(&t, rgb, 138.240); break;
    case SSTV_SCOTTIE2: ok = ok && scottie(&t, rgb, 88.064); break;
    default:            ok = false; break;
    }
    free(rgb);
    if (!ok) {
        free(t.pcm);
        return -21;
    }
    *pcm = t.pcm;
    *count = t.n;
    fprintf(stderr, "SSTV TX: %s, %zu samples at %u Hz (%.3f s)\n", mi->name, t.n, rate, (double)t.n / rate);
    return 0;
}
