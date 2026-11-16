/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax quality mark, see quality.h. The measures are taken on the middle of the fax (rows 12 % to
 *  93 %), clear of the start and stop tones; the mark is a weighted geometric mean of the sub-marks, so that one bad
 *  defect weighs. The medians and filters are numpy's and scipy's, as the station's script has them.
 *
 *  From OlivLoze's fork (F4OLI).
 */

#include "quality.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DARK        128
#define MAX_SHIFT   40

static float clip01(float x)
{
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

/* numpy's median: the mean of the two middle values when n is even */
static float median_f(float *v, unsigned n)
{
    if (!n)
        return 0.0f;
    qsort(v, n, sizeof(*v), cmp_float);
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

/* The value of rank k (0 = lowest) of a histogram */
static int hist_rank(const unsigned *h, unsigned k)
{
    unsigned c = 0;

    for (int v = 0; v < 256; v++) {
        c += h[v];
        if (c > k)
            return v;
    }
    return 255;
}

/* numpy's median of the counted values */
static float hist_median(const unsigned *h, unsigned n)
{
    if (!n)
        return NAN;
    return n % 2 ? (float)hist_rank(h, n / 2) : (hist_rank(h, n / 2 - 1) + hist_rank(h, n / 2)) / 2.0f;
}

/* scipy's gaussian_filter1d (mode reflect, truncate 4) */
static void gauss_row(const float *in, float *out, unsigned w, float sigma)
{
    int   rad = (int)(4.0f * sigma + 0.5f);
    float k[2 * 32 + 1], sum = 0.0f;

    for (int i = -rad; i <= rad; i++)
        sum += k[i + rad] = expf(-0.5f * i * i / (sigma * sigma));
    for (int i = 0; i <= 2 * rad; i++)
        k[i] /= sum;

    for (int x = 0; x < (int)w; x++) {
        float acc = 0.0f;

        for (int i = -rad; i <= rad; i++) {
            int j = x + i;

            while (j < 0 || j >= (int)w)    /* d c b a | a b c d | d c b a */
                j = j < 0 ? -j - 1 : 2 * (int)w - j - 1;
            acc += k[i + rad] * in[j];
        }
        out[x] = acc;
    }
}

/* The row with its slow changes taken off: what the shift between rows is measured on */
static void highpass_row(const uint8_t *row, unsigned w, float *out, float *t1, float *t2)
{
    for (unsigned x = 0; x < w; x++)
        t1[x] = row[x];
    gauss_row(t1, t2, w, 1.0f);
    gauss_row(t2, t1, w, 8.0f);
    for (unsigned x = 0; x < w; x++)
        out[x] = t2[x] - t1[x];
}

/* The shift (px) that best lays row b under row a */
static int best_shift(const float *a, const float *b, unsigned w)
{
    float best  = -INFINITY;
    int   shift = 0;

    for (int k = -MAX_SHIFT; k <= MAX_SHIFT; k++) {
        int   from = k < 0 ? -k : 0, to = k > 0 ? (int)w - k : (int)w;
        float s    = 0.0f;

        for (int n = from; n < to; n++)
            s += a[n + k] * b[n];
        if (s > best) {
            best  = s;
            shift = k;
        }
    }
    return abs(shift);
}

static bool measure(const uint8_t *img, unsigned w, unsigned rows, wefax_quality_measures_t *m)
{
    unsigned        r0 = (unsigned)(0.12 * rows), r1 = (unsigned)(0.93 * rows), n = r1 - r0;
    const uint8_t  *r  = img + (size_t)r0 * w;
    float          *mean = malloc(n * sizeof(float)), *tmp = malloc(n * sizeof(float));
    float          *p90 = malloc(n * sizeof(float)), *white_row = malloc(n * sizeof(float));
    float          *coh = malloc(n * sizeof(float));
    float          *xa = malloc(w * sizeof(float)), *xb = malloc(w * sizeof(float));
    float          *t1 = malloc(w * sizeof(float)), *t2 = malloc(w * sizeof(float));
    bool            ok = mean && tmp && p90 && white_row && coh && xa && xb && t1 && t2 && n >= 2 && w >= 16;
    unsigned        dark_h[256] = {0};
    unsigned        ink = 0, dark_n = 0;

    memset(m, 0, sizeof(*m));
    m->lines = rows;
    if (!ok)
        goto out;

    /* per row: mean, 90th percentile, background (median of the light pixels) */
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *p = r + (size_t)i * w;
        unsigned       h[256] = {0}, sum = 0, light = 0;

        for (unsigned x = 0; x < w; x++) {
            h[p[x]]++;
            sum += p[x];
        }
        mean[i] = (float)sum / w;

        float k  = 0.9f * (w - 1);
        int   lo = (int)k, a = hist_rank(h, lo), b = hist_rank(h, lo + 1 < (int)w ? lo + 1 : lo);

        p90[i] = a + (b - a) * (k - lo);

        for (int v = 0; v < 256; v++) {
            if (v < DARK) {
                dark_h[v] += h[v];
                dark_n += h[v];
            } else if (v > DARK) {
                light += h[v];
            }
            if (v < 110)
                ink += h[v];
        }
        if (light) {
            unsigned hl[256] = {0};

            memcpy(hl + DARK + 1, h + DARK + 1, (255 - DARK) * sizeof(unsigned));
            white_row[i] = hist_median(hl, light);
        } else {
            white_row[i] = 255.0f;
        }
    }

    /* fading: the row mean against the median of the 41 rows around it */
    unsigned faded = 0;

    for (unsigned i = 0; i < n; i++) {
        unsigned k = 0;

        for (int j = (int)i - 20; j <= (int)i + 20; j++)
            tmp[k++] = mean[j < 0 ? 0 : j >= (int)n ? n - 1 : (unsigned)j];
        if (fabsf(mean[i] - median_f(tmp, k)) > 25.0f)
            faded++;
    }
    m->fading = (float)faded / n;

    memcpy(tmp, p90, n * sizeof(float));
    m->white      = median_f(tmp, n);
    m->contrast = dark_n ? m->white - hist_median(dark_h, dark_n) : 0.0f;
    m->ink     = (float)ink / ((float)n * w);

    /* coherence: correlation of the dark pixels of a row with the next one */
    unsigned prev_n = 0, isolated = 0;

    for (unsigned i = 0; i < n; i++) {
        const uint8_t *p = r + (size_t)i * w;
        unsigned       c = 0;

        for (unsigned x = 0; x < w; x++)
            c += p[x] < DARK;
        if (i) {
            const uint8_t *q  = p - w;
            unsigned       both = 0;

            for (unsigned x = 0; x < w; x++)
                both += (p[x] < DARK) & (q[x] < DARK);

            double cov = both - (double)c * prev_n / w;
            double va  = c - (double)c * c / w, vb = prev_n - (double)prev_n * prev_n / w;

            coh[i - 1] = (float)(cov / (sqrt(va * vb) + 1e-6));
        }
        prev_n = c;
    }
    m->coherence = median_f(coh, n - 1);

    /* crackle: dark pixels without a dark neighbour; clean 16 x 16 blocks */
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *p = r + (size_t)i * w, *up = p - w, *down = p + w;

        for (unsigned x = 0; x < w; x++) {
            if (p[x] >= DARK)
                continue;
            if ((i > 0 && up[x] < DARK) || (i + 1 < n && down[x] < DARK) || (x > 0 && p[x - 1] < DARK) ||
                (x + 1 < w && p[x + 1] < DARK))
                continue;
            isolated++;
        }
    }
    m->isolated = (float)isolated / (dark_n ? dark_n : 1);

    unsigned bh = n / 16, bw = w / 16, clean = 0;

    for (unsigned by = 0; by < bh; by++)
        for (unsigned bx = 0; bx < bw; bx++) {
            unsigned d = 0;

            for (unsigned y = 0; y < 16; y++) {
                const uint8_t *p = r + (size_t)(by * 16 + y) * w + bx * 16;

                for (unsigned x = 0; x < 16; x++)
                    d += p[x] < DARK;
            }
            if (d < 0.12f * 256)
                clean++;
        }
    m->clean = bh && bw ? (float)clean / (bh * bw) : 0.0f;

    /* slip: the best shift of row i+1 against row i, one row in 3 */
    unsigned pairs = 0, slipped = 0;

    for (unsigned i = 0; i + 1 < n; i += 3) {
        highpass_row(r + (size_t)i * w, w, xb, t1, t2);
        highpass_row(r + (size_t)(i + 1) * w, w, xa, t1, t2);
        slipped += best_shift(xa, xb, w) > 3;
        pairs++;
    }
    m->slip = pairs ? (float)slipped / pairs : 0.0f;

    /* stripes: the background jumps from a row to the next */
    unsigned jumps = 0;

    for (unsigned i = 1; i < n; i++)
        jumps += fabsf(white_row[i] - white_row[i - 1]) > 4.0f;
    m->stripes = (float)jumps / (n - 1);

out:
    free(mean);
    free(tmp);
    free(p90);
    free(white_row);
    free(coh);
    free(xa);
    free(xb);
    free(t1);
    free(t2);
    return ok;
}

float wefax_quality(const uint8_t *img, unsigned width, unsigned rows, bool no_stop, wefax_quality_measures_t *mp)
{
    wefax_quality_measures_t m;

    if (!measure(img, width, rows, &m)) {
        if (mp)
            *mp = m;
        return 0.0f;
    }
    if (mp)
        *mp = m;

    static const float weight[7] = {.25f, .25f, .10f, .10f, .10f, .10f, .05f};
    float              s[7];

    s[0] = clip01((m.coherence - .20f) / .35f);
    s[1] = fmaxf(.05f, clip01(1 - (m.stripes - .02f) / .25f));
    s[2] = clip01(1 - (m.fading - .02f) / .15f);
    s[3] = clip01(1 - m.slip / .2f);
    s[4] = fminf(clip01(1 - (m.isolated - .01f) / .04f), clip01(m.clean / .4f));
    s[5] = clip01((m.white - 150) / 80) * clip01((m.contrast - 100) / 100);
    s[6] = clip01(1 - (m.ink - .10f) / .15f);

    float g = 0.0f;

    for (int i = 0; i < 7; i++)
        g += weight[i] * logf(fmaxf(s[i], .02f));

    float comp = fminf(1.0f, m.lines / 1300.0f);

    comp *= comp * (no_stop ? 0.8f : 1.0f);
    return 100.0f * expf(g) * comp;
}

float wefax_quality_rows(const uint8_t *img, unsigned w, unsigned rows, float *dark)
{
    float   *c = rows > 1 ? malloc((rows - 1) * sizeof(float)) : NULL;
    unsigned prev = 0, all = 0;

    if (dark)
        *dark = 1.0f;
    if (!c)
        return 0.0f;
    for (unsigned i = 0; i < rows; i++) {
        const uint8_t *p = img + (size_t)i * w;
        unsigned       n = 0;

        for (unsigned x = 0; x < w; x++)
            n += p[x] < DARK;
        if (i) {
            const uint8_t *q    = p - w;
            unsigned       both = 0;

            for (unsigned x = 0; x < w; x++)
                both += (p[x] < DARK) & (q[x] < DARK);

            double cov = both - (double)n * prev / w;
            double va  = n - (double)n * n / w, vb = prev - (double)prev * prev / w;

            c[i - 1] = (float)(cov / (sqrt(va * vb) + 1e-6));
        }
        prev = n;
        all += n;
    }
    if (dark)
        *dark = (float)all / ((float)rows * w);

    float m = median_f(c, rows - 1);

    free(c);
    return m;
}
