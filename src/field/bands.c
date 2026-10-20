/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - band overview
 */

#include "bands.h"

#include "../cluster/client.h"
#include "../cluster/spots.h"

#include <string.h>
#include <time.h>

static const char *const names[FIELD_RBN_BANDS] = {
    "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m",
};

int field_band_of(int32_t hz) {
    const char *name = cluster_band_name(cluster_band_of(hz));

    for (int i = 0; i < FIELD_RBN_BANDS; i++)
        if (!strcmp(name, names[i]))
            return i;
    return -1;
}

const char *field_band_name(int band) {
    return band >= 0 && band < FIELD_RBN_BANDS ? names[band] : "";
}

void field_bands(field_bands_t *b) {
    static cluster_spot_t snap[CLUSTER_MAX_SPOTS];
    static int            snap_n;
    static uint32_t       gen;
    field_rbn_hit_t       hits[32];
    int                   n, best_score = 0, max = 1;
    struct tm             tm;
    time_t                now = time(NULL);

    memset(b, 0, sizeof(*b));
    b->best = -1;
    for (int i = 0; i < FIELD_RBN_BANDS; i++)
        b->best_snr[i] = -99;

    n = cluster_client_snapshot(snap, CLUSTER_MAX_SPOTS, &gen);
    if (n >= 0)
        snap_n = n;
    gmtime_r(&now, &tm);
    for (int k = 0; k < snap_n; k++) {
        int i = field_band_of(snap[k].freq_hz);
        if (i >= 0 && i < FIELD_RBN_BANDS && cluster_age_min(snap[k].utc_min, tm.tm_hour * 60 + tm.tm_min) <= 30)
            b->spots[i]++;
    }
    field_rbn_activity(b->rbn);
    n = field_rbn_heard(FIELD_HEARD_S, hits, 32);
    for (int k = 0; k < n; k++) {
        int i = field_band_of(hits[k].freq_hz);
        if (i >= 0 && i < FIELD_RBN_BANDS) {
            b->heard[i]++;
            if (hits[k].snr > b->best_snr[i])
                b->best_snr[i] = hits[k].snr;
        }
    }
    /* Activity: a cluster spot weighs as 5 skimmer spots; who hears you counts most for the best band */
    for (int i = 0; i < FIELD_RBN_BANDS; i++)
        if (b->spots[i] * 5 + b->rbn[i] > max)
            max = b->spots[i] * 5 + b->rbn[i];
    for (int i = 0; i < FIELD_RBN_BANDS; i++) {
        int act  = b->spots[i] * 5 + b->rbn[i];
        int score = b->heard[i] * 1000 + b->spots[i] * 10 + b->rbn[i];

        b->level[i] = act * 5 / max + (act ? 1 : 0);
        if (score > best_score) {
            best_score = score;
            b->best    = i;
        }
    }
}

int32_t field_band_freq(int band, bool cw) {
    static const int32_t freq[FIELD_RBN_BANDS][2] = {
        {1810000, 1850000},   {3560000, 3760000},   {5355000, 5360000},   {7030000, 7118000},
        {10118000, 10118000}, {14060000, 14285000}, {18086000, 18130000}, {21060000, 21285000},
        {24906000, 24950000}, {28060000, 28400000}, {50060000, 50150000},
    };
    return band >= 0 && band < FIELD_RBN_BANDS ? freq[band][cw ? 0 : 1] : 0;
}

const char *field_band_word(int level) {
    return level >= 5 ? "HIGH" : level >= 4 ? "GOOD" : level >= 2 ? "FAIR" : level ? "POOR" : "-";
}
