/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the rules of the MSG > CW beacon (#71), without the radio: the elements of a text in QRSS3, where
 *  QRSS3 goes on 30 m and 80 m, and when a beacon starts and ends. cw_encoder.c keys them, cw_messages.c shows them,
 *  tools/beacon tests them on the PC.
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

/* QRSS3, slow CW for receivers that draw a long waterfall (the grabbers): a dot 3 s, a dash 9 s, 3 s between the
 * elements of a character, 9 s between characters, 21 s between words */
#define QRSS3_DOT_MS        3000
#define QRSS3_DASH_MS       (3 * QRSS3_DOT_MS)
#define QRSS3_WORD_MS       (7 * QRSS3_DOT_MS)

/* One element: the key down for on_ms, then up for off_ms (0 after the last one) */
typedef struct {
    uint32_t on_ms;
    uint32_t off_ms;
} cw_el_t;

/* The elements of text in QRSS3, in order, at most max of them into out (NULL: only counted); the number there are.
 * The characters of the Morse table as the keyer sends them (upper or lower case, prosigns as <AR>); a space, or a
 * character the table does not have, is a word space. Spaces at the start and at the end are not sent, and a run of
 * them is one word space */
int qrss3_plan(const char *text, cw_el_t *out, int max);

/* How long the elements take, ms: from the first key down to the last key up */
uint32_t cw_plan_ms(const cw_el_t *el, int n);

/* QRSS3's TX time limit, on the radio thread (radio.c): the GUI's key, down since down_ms (the low 32 bits of
 * get_time(), made odd so that it is never 0; 0: up), held longer than limit_ms (0: no limit) at now_ms. A key that
 * went down after now_ms was read, on another thread or later in the same millisecond, has not been held at all */
bool key_limit_over(uint32_t now_ms, uint32_t down_ms, uint32_t limit_ms);

/* QRSS3 on a band: the window the grabbers watch, the frequency in it the radio transmits on when the user chose no
 * other (in CW the carrier is on the frequency shown), the band's edges the user may choose one between, and where
 * the grabbers' receivers are tuned in USB */
typedef struct {
    const char *name;
    int32_t     lo_hz, hi_hz;
    int32_t     win_lo_hz, win_hi_hz;
    int32_t     freq_hz;
    int32_t     dial_hz;
} qrss_band_t;

#define QRSS_BANDS      2       /* 30 m, 80 m */
#define QRSS_STEP_HZ    10

extern const qrss_band_t qrss_bands[QRSS_BANDS];

/* The one of qrss_bands hz is on, -1: none */
int qrss_band_of(int32_t hz);

/* Where QRSS3 goes on band: the frequency the user chose (0: none, the band's), kept on the band */
int32_t qrss_freq(int band, int32_t chosen);

/* That frequency moved by steps of QRSS_STEP_HZ, kept on the band; the band's own gives 0 back, so that a change of
 * the preset reaches it */
int32_t qrss_freq_step(int band, int32_t chosen, int steps);

/* A beacon's schedule (MSG > CW, page 3), on the screen clock: the radio's local time, in the time zone of Settings >
 * General > DATE / TIME (UTC unless one is set). It starts at once or at a time, and ends when stopped, after a number
 * of sends, or at a time: no send starts at or after it, the one on the air then is finished */
typedef enum { BCN_UNTIL_STOP = 0, BCN_UNTIL_SENDS, BCN_UNTIL_TIME } bcn_until_t;

#define BCN_NOW         -1      /* start_min: at once */
#define BCN_STEP_MIN    5       /* the MFK moves a time by 5 minutes */
#define BCN_SENDS_MAX   99

typedef struct {
    int start_min;      /* the minute of the day it starts, BCN_NOW */
    int until;          /* bcn_until_t */
    int sends;          /* BCN_UNTIL_SENDS: how many, 1 to BCN_SENDS_MAX */
    int end_min;        /* BCN_UNTIL_TIME: the minute of the day no send starts from */
} bcn_sched_t;

/* The next time the clock shows the minute min of the day: now while it shows it already */
time_t bcn_next(time_t now, int min);

/* When the first send starts: now, or at its start time */
time_t bcn_start_at(const bcn_sched_t *s, time_t now);

/* BCN_UNTIL_TIME: the first time after the start the clock shows the end (so 24 hours on for the same minute); else 0 */
time_t bcn_end_at(const bcn_sched_t *s, time_t start);

/* The send number done (1 the first) ended now: when the next one starts, after the pause; 0: the beacon is over, its
 * sends all sent, or the next would start at or after the end */
time_t bcn_after_send(const bcn_sched_t *s, time_t end, int done, time_t now, int pause_s);

/* The MFK's steps. The start: at once, then every 5 minutes of a day from the first one after now, round. The end:
 * every 5 minutes of a day from the first one after `from` (the start, or now), round. The sends: 1 to 99 */
int bcn_step_start(int cur, int steps, int now_min);
int bcn_step_end(int cur, int steps, int from_min);
int bcn_step_sends(int cur, int steps);

/* The minute of the day of t on the screen clock */
int bcn_min_of(time_t t);

#ifdef __cplusplus
}
#endif
