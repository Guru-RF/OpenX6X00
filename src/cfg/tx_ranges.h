/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The TX ranges (#247): where the radio may transmit, Settings > TX Cal > TX ranges. Ham bands: the amateur bands of
 *  ham_bands.c, as since #121, with TX outside ham bands to go past them. South Korea: the Korean amateur ranges,
 *  compiled in, for a radio that has to transmit only there to be approved. Own list: ranges read from a file on the
 *  DATA partition, for another country's. The narrower one always wins: TX outside ham bands never takes the radio
 *  outside South Korea's ranges or the own list, and the own list stays inside the amateur bands unless that switch
 *  is on too. Checked on what the radio itself sends out: on a transverter's range its IF (the frequency less the
 *  transverter's shift), so a shift cannot put the radio's own output outside the ranges. tools/tx_ranges checks it.
 */

#pragma once

#include "ham_bands.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Settings > TX Cal > TX ranges (p_tx_ranges), in the order of its list */
typedef enum {
    TX_RANGES_BANDS = 0,    /* Ham bands: the amateur bands (ham_bands.c), the default */
    TX_RANGES_KR,           /* South Korea's amateur ranges */
    TX_RANGES_OWN,          /* the own list (tx_ranges_own_load) */
} tx_ranges_t;

/* The most ranges the own list keeps; the lines after them are not read */
#define TX_RANGES_OWN_MAX   64

/* What the radio itself transmits on, Hz (board.h's BOARD_HF_MIN_HZ and BOARD_HF_MAX_HZ): a line of the own list
 * outside it is not read, it could never transmit */
#define TX_RANGES_MIN_HZ    100000
#define TX_RANGES_MAX_HZ    55000000

/* What the last read of the own list found */
typedef struct {
    int  count;         /* ranges in use, those that overlap or touch joined into one */
    int  bad;           /* lines that are not a range (comments and blank lines aside), and ranges past the most */
    int  first_bad;     /* the first of those, its line number from 1; 0 none */
    bool missing;       /* no file (or it could not be opened): no ranges */
} tx_ranges_own_t;

/* One line of the own list: "7.000-7.200" in MHz, up to 6 decimals (1 Hz), both edges in; spaces around the numbers
 * and the dash, a # and what follows it a comment. 1: a range, into *lo and *hi (Hz); 0: nothing on it (blank, a
 * comment); -1: not a range (no dash, a comma for a point, kHz, from above to, outside 0.1-55 MHz) */
int tx_ranges_line(const char *line, int32_t *lo, int32_t *hi);

/* The own list from text, a line each: its ranges become the list in use, sorted, those that overlap or touch joined.
 * None (no line read) is an empty list: the radio transmits nowhere with Own list chosen. Any thread */
tx_ranges_own_t tx_ranges_own_set(const char *text);
/* The same from a file. A missing one, or one that cannot be read, is an empty list too (missing set) */
tx_ranges_own_t tx_ranges_own_load(const char *path);
/* What the last of those two found; before either, an empty list that is missing */
tx_ranges_own_t tx_ranges_own_state(void);
/* The own list in use, for the test and the log: range i of the count tx_ranges_own_state gives; false past it */
bool tx_ranges_own_get(int i, int32_t *lo, int32_t *hi);

/* The range of profile p that hz lies in, its edges into *lo and *hi (Hz, both in; either may be NULL). Amateur bands:
 * ham_band_of. False: none. Any thread */
bool tx_ranges_of(tx_ranges_t p, int32_t hz, int32_t *lo, int32_t *hi);

/* Why the radio may not transmit at hz. hw_tx: the hardware transmits there (band_use). shift: the shift of the
 * transverter that covers hz, 0 none: the profile is checked at hz - shift, the IF. any: TX outside ham bands on.
 * Amateur bands: ham_tx_check. South Korea and Own list: receive only where the hardware does not transmit, else
 * HAM_TX_NO_RANGES for an empty own list, HAM_TX_OFF_RANGE outside the profile, then ham_tx_check as before (which
 * keeps the own list inside the amateur bands with the switch off). Any thread */
ham_tx_t tx_ranges_check(tx_ranges_t p, int32_t hz, bool hw_tx, int32_t shift, bool any);

/* [*lo, *hi], the frequencies transmitted on the same way as freq (cfg_tx_range), no wider than the profile's range
 * that freq - shift lies in, moved up by shift. False: freq - shift is in none of them (nothing to sweep). Amateur
 * bands: true, left as given. Only ever narrows */
bool tx_ranges_narrow(tx_ranges_t p, int32_t freq, int32_t shift, int32_t *lo, int32_t *hi);

/* The words of a refusal, "outside the TX ranges (Settings > TX Cal)" (cfg_tx_refusal); NULL for HAM_TX_OK */
const char *tx_ranges_why(ham_tx_t why);

/* South Korea's table, for the test and the manual: how many ranges, and range i's edges */
int  tx_ranges_kr_count(void);
void tx_ranges_kr_get(int i, int32_t *lo, int32_t *hi);

#ifdef __cplusplus
}
#endif
