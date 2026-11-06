/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The NCDXF/IARU International Beacon Project: 18 beacons around the world take turns on 14.100,
 *  18.110, 21.150, 24.930 and 28.200 MHz, 10 s each, a round of all 18 every 3 minutes, always at
 *  the same second of the hour. Each sends its call in CW at 100 W, then four 1 s dashes at 100,
 *  10, 1 and 0.1 W. Which one is on the air where at a time is fixed, so a clock is enough to know
 *  who is calling; the S-meter says whether it is heard here, and how well.
 *
 *  The schedule and the scoring of each 10 s slot; no LVGL (host test: tools/propagation/prop_test.c)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BEACONS_N       18
#define BEACON_BANDS    5
#define BEACON_SLOT_MS  10000
#define BEACON_ROUND_S  180         /* the 18 slots */

typedef struct {
    const char *call;
    const char *where;
    const char *grid;
} beacon_t;

extern const beacon_t beacons[BEACONS_N];
extern const int32_t  beacon_freq[BEACON_BANDS];        /* Hz, in the order of the round */
extern const char    *beacon_band_name[BEACON_BANDS];   /* "20m"... */

/* The beacon on band b at a time (UTC seconds), and the band beacon i is on (-1: none of the five) */
int beacon_on(int band, time_t t);
int beacon_band_of(int beacon, time_t t);

/* The band of a frequency, -1 when it is not one of the five (within 500 Hz) */
int beacon_band_at(int32_t freq);

/* ---- What is heard ---- */

#define BEACON_NONE  INT8_MIN    /* not listened to */

typedef struct {
    int8_t snr;         /* dB over the band's noise when heard, BEACON_NONE never listened to */
    bool   heard;
    time_t when;        /* the slot's start */
} beacon_heard_t;

void beacons_reset(void);

/* An S-meter reading (dB) at time ms (UTC milliseconds) on band. Readings in the first half
 * second of a slot are left out (the one before may still be keying down), as is a slot that
 * changed band. A slot is scored once the next one starts */
void beacons_sample(int64_t ms, int band, float db);

/* A retune or TX at ms (UTC milliseconds): the slot under way stops counting, unless it is in
 * its first half second (left out anyway), and the slot before, when it is over, is scored */
void beacons_retune(int64_t ms);

const beacon_heard_t *beacons_heard(int beacon, int band);

/* The band's noise as the scoring takes it (its loud readings, the 90th percentile), dB; NAN
 * until a few slots are in */
float beacons_floor(int band);

/* The weakest of the four dashes a heard SNR (of the 100 W part) would still carry: "100 W",
 * "10 W", "1 W" or "0.1 W". Each dash is 10 dB under the one before; 6 dB over the noise is
 * taken as readable */
const char *beacon_power_heard(int snr);

#ifdef __cplusplus
}
#endif
