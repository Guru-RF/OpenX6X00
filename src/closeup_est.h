/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The close-up's "off by" estimate (closeup_view.c): how far the strongest signal in the passband is from where it
 *  should be. No LVGL: tested on the PC (tools/wf3d)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CLOSEUP_GREEN_HZ    20      /* this close: centred */
#define CLOSEUP_EST_BINS    256     /* a row's bins at most */
#define CLOSEUP_EST_WARMUP  4       /* rows averaged before an estimate */

typedef struct {
    float   off_hz;     /* smoothed */
    bool    valid;      /* a signal seen lately */
    uint8_t quiet;      /* rows since the last one with a signal */
    int     n;          /* the rows' bins averaged in avg; 0: none yet */
    uint32_t span;      /* and their width: rows of another (the span switched) start the average again */
    uint8_t rows;       /* rows in it, to CLOSEUP_EST_WARMUP: no estimate before (one row's noise peaks passed) */
    float   avg[CLOSEUP_EST_BINS];  /* the rows' power, averaged (0.2 new): a row alone is a few FFTs, its noise
                                     * reached 10 dB over its median here and there */
} closeup_est_t;

/* db: n bins over span_hz centred on the row's centre. pb_lo, pb_hi: the passband in Hz from that centre; want:
 * where the signal's centre should be, in Hz from it. The rows are averaged first. The signal: the energy
 * centroid of the bins on from the passband's peak while within 6 dB of it (past the passband's edges too), when
 * that peak is at least 10 dB over the noise (the 10th percentile: a voice may fill most of a row). edge, for SSB: 1 (USB) want is where the voice's lower edge should be (300 Hz over the dial),
 * the edge from the bins under the peak while 6 dB over the noise; -1 (LSB) the upper one. off_hz (in frequency:
 * the signal higher, more) smoothed (0.3 new, 0.7 old; the first at once); valid false after 5 rows without a
 * signal */
void closeup_est_update(closeup_est_t *e, const float *db, int n, uint32_t span_hz, int32_t pb_lo, int32_t pb_hi,
                        int32_t want, int edge);
void closeup_est_reset(closeup_est_t *e);
