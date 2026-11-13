/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WSPR band hopping (#190): which band each 2-minute cycle is on, and whether it may carry a beacon
 *
 *  WSJT-X's coordinated hopping: the minute of the hour picks the band, in ten 2-minute slots every 20 minutes (160 m
 *  at :00, 80 m :02, 60 m :04, 40 m :06, 30 m :08, 20 m :10, 17 m :12, 15 m :14, 12 m :16, 10 m :18, again from :20),
 *  so everyone who hops this way is on the same band at the same time. Each band is set to Off, RX or RX+TX. A slot
 *  whose band is Off goes to the other bands that are on, in turn (WSJT-X picks one at random; a fixed turn gives
 *  each the same share and is the same on every radio). 6 m has no slot of its own: when it is on it takes its turn
 *  in the Off slots, as WSJT-X's band list allows. No LVGL, no parameters, no clock: dialog_wspr.c gives the values,
 *  tools/wspr_hop tests them.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WSPR_HOP_BANDS  11      /* 160 80 60 40 30 20 17 15 12 10 6 m, the app's list */
#define WSPR_HOP_SLOTS  10      /* 160 m to 10 m have a slot, 6 m none */
#define WSPR_HOP_CYCLE  120     /* s, one slot */

/* The hop is asked once the cycle is over: a beacon ends at 111.6 s, the decode takes its samples at 114 s
 * (dialog_wspr.c DECODE_S). The next cycle's beacon is made at 116 s (PREPARE_S): the band is known by then */
#define WSPR_HOP_FROM_S 114.5
#define WSPR_HOP_TO_S   116.0

typedef enum {
    WSPR_USE_OFF,
    WSPR_USE_RX,
    WSPR_USE_TX,        /* RX+TX */
} wspr_use_t;

/* Every band's use in one number, two bits a band from 160 m up (the wspr_bands parameter). By default the ten slot
 * bands receive and 6 m is off: a beacon needs a band set to RX+TX first, on an antenna that takes it */
#define WSPR_HOP_DEFAULT 0x55555
#define WSPR_HOP_MAX     ((1 << (2 * WSPR_HOP_BANDS)) - 1)

/* Band i: its name in metres, and WSJT-X's WSPR dial frequency on it (Hz) */
int     wspr_hop_band_m(int band);
int32_t wspr_hop_band_dial(int band);

/* The band hz is on (the amateur band's edges, cfg/ham_bands.c), -1 for none of the eleven */
int wspr_hop_band_of(int32_t hz);

/* A band's use in bands; a pair of bits that is no use (3) counts as Off, as does a band outside the list */
wspr_use_t wspr_hop_use(int32_t bands, int band);
int32_t    wspr_hop_set_use(int32_t bands, int band, wspr_use_t use);

/* Whether a band is set to use, or more (RX: RX or RX+TX) */
bool wspr_hop_any(int32_t bands, wspr_use_t use);

/* The next use for the use button: Off, RX, RX+TX, Off... */
wspr_use_t wspr_hop_next_use(wspr_use_t use);

/* "Off", "RX", "RX+TX" */
const char *wspr_hop_use_name(wspr_use_t use);

/* The coordinated slot's band for the cycle that starts at cycle (UTC seconds, a multiple of 120) */
int wspr_hop_slot(time_t cycle);

/* The band the cycle that starts at cycle is on: the slot's band when it is not Off, else the next of the other
 * bands that are on, in turn; -1 when every band is Off */
int wspr_hop_pick(int32_t bands, time_t cycle);

/* Whether a beacon may go out on band: RX+TX, and with the ATU on a match stored for the band's WSPR frequency (the
 * radio sets that match again when it comes back there; without one the hop receives only, no tuning carrier is sent
 * by itself) */
bool wspr_hop_tx_ok(int32_t bands, int band, bool atu_on, bool atu_match);

/* Whether the band may change at t s into the cycle: only between two cycles, after the beacon and after the decode
 * took this cycle's samples, never while a beacon is on the air (on_air) or a cycle is being recorded (recording) */
bool wspr_hop_window(double t, bool on_air, bool recording);

/* Each band's WSPR frequency from the app's frequency list (presets, n of them, Hz): the one closest to WSJT-X's on
 * that band, 0 for a band the list has none on */
void wspr_hop_freqs(const int32_t *presets, int n, int32_t freqs[WSPR_HOP_BANDS]);

/* bands with every band the list has no frequency on set to Off: the hop cannot go there */
int32_t wspr_hop_usable(int32_t bands, const int32_t freqs[WSPR_HOP_BANDS]);

#ifdef __cplusplus
}
#endif
