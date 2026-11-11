/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The amateur bands the radio transmits on by default (#121): one table for every country, 0.1-55 MHz, the union of
 *  the ITU Radio Regulations Article 5 amateur allocations of Regions 1-3 and the wider national bands. The licence
 *  decides inside it; the table only stops clear out-of-band TX (CB, marine, broadcast, MARS and other channels
 *  outside the bands). tools/ham_bands checks the edges.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why the radio may not transmit at a frequency */
typedef enum {
    HAM_TX_OK = 0,
    HAM_TX_RX_ONLY,     /* the hardware does not transmit there (the X6200's FM broadcast and airband, the WR12, above
                         * HF without a transverter, under 0 Hz with a TX shift), whatever the setting */
    HAM_TX_OFF_BAND,    /* HF outside the amateur bands, with TX outside them off */
} ham_tx_t;

/* The amateur band hz is in, its edges into *lo and *hi (Hz, both in; either may be NULL); false: none. Any thread */
bool ham_band_of(int32_t hz, int32_t *lo, int32_t *hi);

/* hw_tx: the hardware transmits at hz (band_use). xvtr: a transverter that is on covers hz. any: Settings > TX Cal >
 * TX outside ham bands on */
ham_tx_t ham_tx_check(int32_t hz, bool hw_tx, bool xvtr, bool any);

/* What the SWR scan sweeps on the amateur band freq is in: [*lo, *hi] no wider than the band list's active rows (start
 * and stop, Hz, n of them) that lie inside that band. Those follow the IARU region where this table is the union of all
 * three: in Region 1 40 m is 7.000-7.200 (40m CW and 40m SSB), 80 m 3.500-3.800, where the table goes to 7.300 and
 * 4.000. A row reaching outside the band (Broadcast 41m from 7.200) is not one of them, nor an inactive one (Broadcast
 * 75m, 3.900-4.000, inside 80 m). Left as given: freq on no amateur band, no active row inside it, or edges that would
 * not overlap what was given. Only ever narrows */
void ham_band_scan_range(int32_t freq, const int32_t *start, const int32_t *stop, int n, int32_t *lo, int32_t *hi);

/* The table, for tools/ham_bands: how many bands, and band i's edges */
int  ham_bands_count(void);
void ham_bands_get(int i, int32_t *lo, int32_t *hi);

#ifdef __cplusplus
}
#endif
