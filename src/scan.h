/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The scanner's engine: through a list of frequencies, a short look at each, a stop on one that
 *  is busy, and on again when it has been quiet a while (the hang time), as a scanner does.
 *
 *  Busy is a level over a threshold: a fixed S-meter level, or Auto, a margin over what each
 *  frequency reads when it is quiet. Auto learns that as it goes: the lowest reading of each,
 *  let up a little on every visit, so a band that gets noisier does not stop it everywhere.
 *  The first visit of a frequency only learns, unless enough of the others are known already.
 *
 *  No LVGL and no radio here: the window tunes what scan_tick() says and hands in the S-meter
 *  readings (host/tests/scan_test.c)
 */

#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCAN_MAX      1000
#define SCAN_AUTO     NAN       /* stop_db: a margin over each frequency's quiet level */
#define SCAN_HITS_MAX 40

typedef enum {
    SCAN_STOPPED,
    SCAN_SEEKING,   /* looking at one, on to the next */
    SCAN_HOLD,      /* stopped on a busy one */
    SCAN_PAUSED,    /* held by hand */
} scan_state_t;

typedef struct {
    int     idx;
    time_t  start;      /* wall clock */
    int     secs;       /* how long it stayed busy */
    float   peak;       /* dB */
} scan_hit_t;

/* A new list of n frequencies: no levels learnt, none locked out, stopped */
void scan_init(int n);

/* Look dwell_ms at each (the first half is left to the receiver to settle), stay hang_ms after
 * the last busy reading; stop_db: SCAN_AUTO or a level in dB, margin_db over the quiet level
 * in Auto */
void scan_config(int dwell_ms, int hang_ms, float stop_db, float margin_db);

/* From i (the next one not locked out) */
void scan_start(int i, int64_t now_ms);
void scan_stop(void);
void scan_pause(bool on, int64_t now_ms);

/* Every few ms: the index to tune now, -1 when nothing changes */
int scan_tick(int64_t now_ms, time_t wall);

/* An S-meter reading, dB */
void scan_reading(int64_t now_ms, float db);

/* Away from the one it holds, to the next */
void scan_skip(int64_t now_ms);

void scan_lockout(int i, bool on);
bool scan_locked(int i);

scan_state_t scan_state(void);
int          scan_current(void);            /* tuned now, -1 none */
float        scan_threshold(int i);         /* what counts as busy on i now, NAN unknown */
float        scan_quiet(int i);             /* its quiet level learnt, NAN unknown */
int          scan_passes(void);             /* rounds of the list done */

/* The busy ones, the latest first */
int               scan_hits_n(void);
const scan_hit_t *scan_hit(int k);

#ifdef __cplusplus
}
#endif
