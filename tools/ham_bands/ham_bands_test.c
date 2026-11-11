/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cfg/ham_bands.c (#121): the table row by row against the edges written out here (the manual's
 *  table, docs/MANUAL.md "Amateur bands only"), each of those edges in and 1 Hz past it out, the table sorted and with
 *  no two bands touching, frequencies in and out of the bands (CB, marine, broadcast, the channels some countries
 *  have outside them), and ham_tx_check's answers for the hardware, a transverter and the switch.
 */

#include "cfg/ham_bands.h"

#include <stdio.h>

static int fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* The edges, Hz, both in: what the table must hold, written out, not read back from it */
static const struct {
    int32_t lo, hi;
} want[] = {
    {135700, 137800},       /* 2200 m */
    {472000, 479000},       /* 630 m */
    {1800000, 2000000},     /* 160 m */
    {3500000, 4000000},     /* 80 m */
    {5167000, 5168000},     /* Alaska emergency channel, round its 5167.5 kHz carrier */
    {5250000, 5450000},     /* 60 m */
    {7000000, 7300000},     /* 40 m */
    {10100000, 10150000},   /* 30 m */
    {14000000, 14350000},   /* 20 m */
    {18068000, 18168000},   /* 17 m */
    {21000000, 21450000},   /* 15 m */
    {24890000, 24990000},   /* 12 m */
    {28000000, 29700000},   /* 10 m */
    {40000000, 45000000},   /* 8 m */
    {50000000, 54000000},   /* 6 m */
};

#define WANT_N ((int)(sizeof(want) / sizeof(want[0])))

/* In a band: the UK's lowest 60 m block, the 60 m channels, the Alaska channel's carrier, FT8, 8 m's beacon and its
 * ISM slot */
static const int32_t in[] = {
    5167500, 5258500, 5332000, 5351500, 5357000, 5366500, 5403500, 5405000, 7074000, 14074000, 40013000, 40680000,
};

/* Out: under 2200 m and between it and 630 m (old maritime calling), marine distress and the marine bands, Japan's
 * emergency channel, Somalia's 60 m below 5250, broadcast, the CB plans (CEPT and US channel 1, 19, 40; the UK's
 * channel 40), Ireland's blocks outside 8 m, cordless phones, above 6 m, the top of the radio's HF */
static const int32_t out[] = {
    100000, 500000, 2182000, 4125000, 4630000, 5100000, 5170300, 5950000, 6215000, 7350000, 8291000,
    26965000, 27185000, 27405000, 27991250, 35000000, 49830000, 54000001, 54500000, 55000000,
};

/* ham_band_scan_range (the SWR scan's sweep): the bench X6100's band list, its active rows as cfg_tx_scan_range passes
 * them (Region 1 edges; the broadcast rows are inactive and not passed), and a Region 2 list */
static const int32_t r1_start[] = {1810000, 3500000, 5351500, 7000000, 7040000, 10100000, 14000000, 14070000, 18068000,
                                   21000000, 24890000, 26055000, 26515000, 26965000, 27415000, 28000000, 50000000};
static const int32_t r1_stop[]  = {2000000, 3800000, 5366500, 7040000, 7200000, 10150000, 14070000, 14350000, 18168000,
                                   21450000, 24990000, 26295000, 26745000, 27195000, 27645000, 29700000, 53900000};
static const int32_t r2_start[] = {1800000, 3500000, 7000000, 7040000, 50000000};
static const int32_t r2_stop[]  = {2000000, 4000000, 7040000, 7300000, 54000000};
#define R1_N ((int)(sizeof(r1_start) / sizeof(r1_start[0])))
#define R2_N ((int)(sizeof(r2_start) / sizeof(r2_start[0])))

static void scan(const char *what, int32_t freq, const int32_t *start, const int32_t *stop, int n, int32_t lo, int32_t hi,
                 int32_t want_lo, int32_t want_hi) {
    ham_band_scan_range(freq, start, stop, n, &lo, &hi);
    check(lo == want_lo, what, lo);
    check(hi == want_hi, what, hi);
}

static void scan_ranges(void) {
    /* Region 1: the band list's band, where the table has the union of the regions */
    scan("R1 40 m: 7.000-7.200, not 7.300", 7076000, r1_start, r1_stop, R1_N, 7000000, 7300000, 7000000, 7200000);
    scan("R1 40 m from the CW part", 7020000, r1_start, r1_stop, R1_N, 7000000, 7300000, 7000000, 7200000);
    scan("R1 80 m: 3.800, not 4.000", 3700000, r1_start, r1_stop, R1_N, 3500000, 4000000, 3500000, 3800000);
    scan("R1 160 m from 1.810", 1850000, r1_start, r1_stop, R1_N, 1800000, 2000000, 1810000, 2000000);
    scan("R1 60 m: the band list's 5.3515-5.3665", 5360000, r1_start, r1_stop, R1_N, 5250000, 5450000, 5351500, 5366500);
    scan("6 m: the band list's 53.9", 50313000, r1_start, r1_stop, R1_N, 50000000, 54000000, 50000000, 53900000);
    scan("30 m as it was", 10136000, r1_start, r1_stop, R1_N, 10100000, 10150000, 10100000, 10150000);
    scan("20 m: CW and SSB together", 14074000, r1_start, r1_stop, R1_N, 14000000, 14350000, 14000000, 14350000);
    /* Region 2: the same as the table */
    scan("R2 40 m to 7.300", 7250000, r2_start, r2_stop, R2_N, 7000000, 7300000, 7000000, 7300000);
    scan("R2 80 m to 4.000", 3900000, r2_start, r2_stop, R2_N, 3500000, 4000000, 3500000, 4000000);
    /* Left as given */
    scan("CB: no amateur band", 27185000, r1_start, r1_stop, R1_N, 100000, 55000000, 100000, 55000000);
    scan("no rows", 7076000, r1_start, r1_stop, 0, 7000000, 7300000, 7000000, 7300000);
    {
        const int32_t b41_start[] = {7200000}, b41_stop[] = {7500000};
        scan("a row reaching past the band (Broadcast 41m) is not the band", 7100000, b41_start, b41_stop, 1, 7000000,
             7300000, 7000000, 7300000);
    }
    scan("only narrows: a range cut lower stays cut", 7150000, r1_start, r1_stop, R1_N, 7100000, 7300000, 7100000, 7200000);
    scan("no overlap with what was given: as given", 7250000, r1_start, r1_stop, R1_N, 7250000, 7300000, 7250000, 7300000);
    /* R1 dial past the band list's 40 m (tuned by hand to 7.250): the scan keeps to 7.000-7.200 */
    scan("R1 dial at 7.250: the sweep inside 7.000-7.200", 7250000, r1_start, r1_stop, R1_N, 7000000, 7300000, 7000000, 7200000);
}

int main(void) {
    int     n = ham_bands_count();
    int32_t lo, hi, phi = 0;

    check(n == WANT_N, "15 bands", n);
    for (int i = 0; i < n && i < WANT_N; i++) {
        ham_bands_get(i, &lo, &hi);
        check(lo == want[i].lo, "a band's low edge as written", lo);
        check(hi == want[i].hi, "a band's high edge as written", hi);
        check(lo <= hi, "a band's edges in order", lo);
        check(i == 0 || lo > phi + 1, "sorted, not touching the one before", lo);
        check(lo >= 100000 && hi <= 55000000, "inside the radio's HF", lo);
        phi = hi;
    }
    for (int i = 0; i < WANT_N; i++) {
        int32_t wl = want[i].lo, wh = want[i].hi, bl = 0, bh = 0;

        check(ham_band_of(wl, &bl, &bh) && bl == wl && bh == wh, "the low edge in, its band's edges", wl);
        check(ham_band_of(wh, &bl, &bh) && bl == wl && bh == wh, "the high edge in, its band's edges", wh);
        check(!ham_band_of(wl - 1, NULL, NULL), "1 Hz under the low edge out", wl - 1);
        check(!ham_band_of(wh + 1, NULL, NULL), "1 Hz over the high edge out", wh + 1);
    }

    for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        check(ham_band_of(in[i], NULL, NULL), "in a band", in[i]);
    }
    for (size_t i = 0; i < sizeof(out) / sizeof(out[0]); i++) {
        check(!ham_band_of(out[i], NULL, NULL), "outside the bands", out[i]);
    }

    lo = hi = 0;
    check(ham_band_of(7100000, &lo, &hi) && lo == 7000000 && hi == 7300000, "40 m's edges", lo);
    check(ham_band_of(137800, &lo, &hi) && lo == 135700 && hi == 137800, "2200 m's edges", lo);
    check(ham_band_of(54000000, &lo, NULL) && lo == 50000000, "6 m's low edge, hi not asked", lo);
    lo = hi = 1;
    check(!ham_band_of(27185000, &lo, &hi) && lo == 1 && hi == 1, "CB: none, the edges left alone", lo);
    check(!ham_band_of(-850000, NULL, NULL), "under 0 Hz (a TX shift): in no band", -850000);

    check(ham_tx_check(100000000, false, false, false) == HAM_TX_RX_ONLY, "no hardware TX: receive only", 0);
    check(ham_tx_check(100000000, false, true, true) == HAM_TX_RX_ONLY, "receive only, the switch on", 0);
    check(ham_tx_check(145500000, true, true, false) == HAM_TX_OK, "a transverter's range", 145500000);
    check(ham_tx_check(27185000, true, false, true) == HAM_TX_OK, "CB with the switch on", 27185000);
    check(ham_tx_check(27185000, true, false, false) == HAM_TX_OFF_BAND, "CB, limited", 27185000);
    check(ham_tx_check(28074000, true, false, false) == HAM_TX_OK, "10 m, limited", 28074000);
    check(ham_tx_check(29700001, true, false, false) == HAM_TX_OFF_BAND, "past 10 m, limited", 29700001);

    scan_ranges();

    printf("ham_bands: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
