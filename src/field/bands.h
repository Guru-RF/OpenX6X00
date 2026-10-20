/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - band overview for Field Ops and the Bands app: per band the cluster
 *  spots of the last 30 minutes, the RBN skimmer spots of the last minutes and the skimmers that
 *  heard our callsign; the best band; a typical activation frequency of each band.
 */

#pragma once

#include "rbn.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIELD_HEARD_S 600   /* the skimmers of the last 10 minutes */

typedef struct {
    int spots[FIELD_RBN_BANDS];
    int rbn[FIELD_RBN_BANDS];
    int heard[FIELD_RBN_BANDS];
    int best_snr[FIELD_RBN_BANDS];  /* -99: nobody */
    int level[FIELD_RBN_BANDS];     /* 0 (nothing) .. 6 */
    int best;                       /* the band to use, -1 none */
} field_bands_t;

/* Field's bands (indexes 0 .. FIELD_RBN_BANDS - 1, 160 m .. 6 m), looked up by name in the
 * cluster's table: that table may grow or change order without moving these */
int         field_band_of(int32_t hz);
const char *field_band_name(int band);

/* Now (LVGL thread: it reads the cluster spots) */
void field_bands(field_bands_t *b);

/* A typical activation frequency of the band (field_band_of() index): CW or phone */
int32_t field_band_freq(int band, bool cw);

/* "HIGH", "GOOD", "FAIR", "POOR", "-" for a level */
const char *field_band_word(int level);

#ifdef __cplusplus
}
#endif
