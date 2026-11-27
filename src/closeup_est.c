/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The close-up's "off by" estimate (closeup_est.h)
 */

#include "closeup_est.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EST_MAX_BINS    CLOSEUP_EST_BINS
#define EST_AVG_NEW     0.2f
#define EST_WITHIN_DB   6.0f    /* the signal: the bins this close to the passband's peak */
#define EST_OVER_DB     10.0f   /* and the peak this far over the noise */
#define EST_NOISE_AT    10      /* the noise: this percentile of the averaged row */
#define EST_EDGE_DB     6.0f    /* SSB: a voice's edge, where it falls under this over the noise */
#define EST_INSIDE      2       /* SSB: bins inside the edge the voice's level is taken from, to twice that */
#define EST_NEW         0.3f
#define EST_QUIET_ROWS  5

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *) a, y = *(const float *) b;

    return x < y ? -1 : x > y;
}

void closeup_est_reset(closeup_est_t *e) {
    e->off_hz = 0.0f;
    e->valid = false;
    e->quiet = EST_QUIET_ROWS;
    e->n = 0;
}

static void quiet_row(closeup_est_t *e) {
    if (e->quiet < EST_QUIET_ROWS) e->quiet++;
    if (e->quiet >= EST_QUIET_ROWS) e->valid = false;
}

void closeup_est_update(closeup_est_t *e, const float *db, int n, uint32_t span_hz, int32_t pb_lo, int32_t pb_hi,
                        int32_t want, int edge) {
    float s[EST_MAX_BINS], a[EST_MAX_BINS];

    if (n <= 1 || n > EST_MAX_BINS || !span_hz) {
        return;
    }
    /* The rows averaged in power; from here on db is the average's */
    for (int i = 0; i < n; i++) {
        float p = powf(10.0f, db[i] * 0.1f);

        e->avg[i] = e->n == n ? e->avg[i] + (p - e->avg[i]) * EST_AVG_NEW : p;
        a[i] = 10.0f * log10f(e->avg[i] > 1e-30f ? e->avg[i] : 1e-30f);
    }
    e->n = n;
    db = a;
    memcpy(s, db, n * sizeof(float));
    qsort(s, n, sizeof(float), cmp_float);
    float noise = s[n * EST_NOISE_AT / 100];

    /* The passband's bins */
    float hz_per_bin = (float) span_hz / n;
    int   b0 = (int) ceilf(pb_lo / hz_per_bin + n / 2.0f), b1 = (int) floorf(pb_hi / hz_per_bin + n / 2.0f);

    if (b0 < 0) b0 = 0;
    if (b1 > n - 1) b1 = n - 1;
    if (b0 > b1) {
        quiet_row(e);
        return;
    }
    int top = b0;

    for (int i = b0 + 1; i <= b1; i++) {
        if (db[i] > db[top]) top = i;
    }
    float peak = db[top];

    if (peak < noise + EST_OVER_DB) {
        quiet_row(e);
        return;
    }
    float off;

    if (edge) {
        /* SSB: the voice's edge on its carrier's side, the bins on from the peak while over the noise (a voice's
         * energy lies low in it, its middle was hundreds of Hz under the passband's when tuned right) */
        int   i = top;
        float floor_db = noise + EST_EDGE_DB;

        if (edge > 0) {
            while (i > 0 && db[i - 1] >= floor_db) i--;
        } else {
            while (i < n - 1 && db[i + 1] >= floor_db) i++;
        }
        /* That is where the FFT window's skirt meets the noise, its main lobe further out than the voice (200 Hz at
         * 49 Hz a bin). The edge itself: where the level is half (-3 dB) the voice's just inside, EST_INSIDE bins
         * on, a step spread by the window being half way up at its edge */
        float level = -1e30f;

        for (int k = EST_INSIDE; k < 2 * EST_INSIDE; k++) {
            int j = i + edge * k;

            if (j >= 0 && j < n && db[j] > level) level = db[j];
        }
        float at_bin = i - edge * 0.5f;     /* the edge between the bins */

        if (level > -1e30f) {
            float half = level - 3.0f;

            while (i + edge >= 0 && i + edge < n && db[i] < half && (i - top) * edge < 0) i += edge;
            /* between the last bin under it and the first over it, linear in dB */
            int o = i - edge;

            if (o >= 0 && o < n && db[o] < half && db[i] > db[o]) {
                at_bin = o + edge * (half - db[o]) / (db[i] - db[o]);
            } else {
                at_bin = i - edge * 0.5f;
            }
        }
        off = (at_bin - n / 2.0f) * hz_per_bin - want;
        e->off_hz = e->valid ? e->off_hz + (off - e->off_hz) * EST_NEW : off;
        e->valid = true;
        e->quiet = 0;
        return;
    }
    /* The signal: the bins on from the peak while they stay near it, out of the passband too (a signal tuned off
     * reaches over its edge: only its part inside was pulled towards the middle); its energy centroid */
    int lo = top, hi = top;

    while (lo > 0 && db[lo - 1] >= peak - EST_WITHIN_DB) lo--;
    while (hi < n - 1 && db[hi + 1] >= peak - EST_WITHIN_DB) hi++;

    double sum = 0.0, at = 0.0;

    for (int i = lo; i <= hi; i++) {
        double p = pow(10.0, (db[i] - peak) / 10.0);

        sum += p;
        at += p * (i - n / 2.0);
    }
    off = (float)(at / sum) * hz_per_bin - want;

    e->off_hz = e->valid ? e->off_hz + (off - e->off_hz) * EST_NEW : off;
    e->valid = true;
    e->quiet = 0;
}
