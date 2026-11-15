/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Auto Time Sync of the FT8 app (time_sync.c): the clock set from the time offset (DT) of the decodes
 *
 *  For a radio with neither WiFi (NTP) nor a GPS to keep its clock (#261's first step). Wherever there is FT8 or FT4
 *  traffic, the stations' DT says how far the clock is off, to about a tenth of a second. Each decode gives its DT,
 *  measured from the slot's start by the time of its audio: audio_worker.c stamps the decoder's audio as it comes,
 *  since that audio does not begin at the slot's start (it starts again after the slot before's last decode), and
 *  the decoder's own lag (FTX_SYNC_DECODER_LAG) is taken off. One vote a station, the median of its own DTs, so one
 *  busy station, or a Fox, counts once. With FTX_SYNC_MIN_STATIONS stations heard over FTX_SYNC_MIN_SLOTS slots in
 *  the last FTX_SYNC_WINDOW_S, the stations' median is the clock's error once FTX_SYNC_RX_DT_S (the DT a right clock
 *  shows here) is taken off. A step comes only when the stations agree (their spread at most FTX_SYNC_SPREAD_MAX_S)
 *  and the error is at least FTX_SYNC_STEP_MIN_S and at most FTX_SYNC_STEP_MAX_S.
 *
 *  A DT is only good for the clock its audio was stamped with: a decode whose audio began before the clock was set
 *  (this sync, the Time Sync key, the web Time page, ntpd) is not counted, nor kept once the clock is set after it.
 *  CLOCK_REALTIME - CLOCK_MONOTONIC tells: only a step of the clock changes it (a slew moves both).
 *
 *  FT8 and FT4 only: their signals start 0.5 s into the slot (WSJT-X; tx_worker.c sends so). Where an FT2 signal
 *  starts is not settled (the app's own goes as soon as it is keyed): FT2 decodes are not counted.
 *
 *  Here only the decisions, from the times given; ft8_window.c collects the decodes, picks the moment and sets the
 *  clock. Nothing here reads a clock: the host test (tools/ft8_sync) drives it.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FT8 and FT4 transmissions start 0.5 s into their slot, as WSJT-X sends (tx_worker.c's TX_START_NS): DT 0 */
#define FTX_SYNC_START_S        0.5f

/* The decoder (worker.c) gives a signal's time about 1.2 symbols late: each column of its waterfall is the FFT of
 * the two symbols' length up to that point, at its best on a symbol whose middle is the window's middle, and is filed
 * under the window's end less a quarter symbol, 1.25 symbols after that symbol's start. On the app's own signals the
 * decodes come 1.16 (FT8) and 1.19 (FT4) symbols late on average: FT8 0.19 s, FT4 0.06 s (tools/ft8_sync) */
#define FTX_SYNC_DECODER_LAG    1.2f    /* symbols */

/* The median DT (decoder's lag taken off) the decodes show on this radio with a clock that is right. Not 0: a
 * signal reaches the decoder late by the receive path (the base's filters and the codec, the capture buffer
 * PulseAudio keeps under each fragment, the scheduling), and the stations on the air send a little late on average
 * (their own audio paths). The code does not know it: it is the bench's number, the median DT on the bench X6200
 * with an NTP-held clock (with Auto Sync on, ft8_window.c logs it once a minute: "ft8: Auto Time Sync: median DT").
 * Until it is measured, 0.15 s: what a right clock shows on a busy band in the usual desktop programs. Aimed at 0
 * instead, the clock would be set late by it, and the radio would transmit late by it */
#define FTX_SYNC_RX_DT_S        0.15f

/* Enough to go by: 5 stations (two clocks far off among them are outvoted), heard in 3 slots or more (a slot's audio
 * stamp is shared by all its decodes: one bad stamp is outvoted), within 4 minutes (16 FT8 slots, 32 FT4; a clock
 * chip on its own drifts a few seconds a day, nowhere near 0.1 s in that time) */
#define FTX_SYNC_MIN_STATIONS   5
#define FTX_SYNC_MIN_SLOTS      3
#define FTX_SYNC_WINDOW_S       240

/* The clock moves when it is 0.2 s off or more: the median of 5 stations varies by about 0.05 s (their own clocks,
 * the decoder's time step of 40 ms in FT8, 12 ms in FT4), so 0.2 s is no chance swing, and well inside what FT8 and
 * FT4 decode. At most 2 s: with its audio begun 0.2 s into the slot, the decoder finds FT8 signals from about 1.6 s
 * early to 2.8 s late and FT4 ones from 0.5 s early to 2.3 s late, so a median past 2 s is the edge of what it sees,
 * not a measure; the Time Sync key, pressed as a slot starts, is for a clock that far off */
#define FTX_SYNC_STEP_MIN_S     0.2f
#define FTX_SYNC_STEP_MAX_S     2.0f

/* The stations' votes must agree: their median distance from the median at most 0.3 s. More, and the band has two
 * camps (a group of stations whose clocks are off together): no step */
#define FTX_SYNC_SPREAD_MAX_S   0.3f

/* A DT further from 0 than this is no decode to go by (the decoder's search reaches 3.4 s at most) */
#define FTX_SYNC_DT_MAX_S       4.0f

/* CLOCK_REALTIME - CLOCK_MONOTONIC moved more than 10 ms: the clock was set since */
#define FTX_SYNC_SET_NS         10000000LL

/* The moment: within 1 s after the middle of a slot, and the clock after the step still 1 s or more inside the same
 * slot. A transmission of this slot started at 0.5 s or not at all, and one under way holds the decoder thread until
 * its end, so a slot we send in has its middle go by unchecked. The decoder sees no slot start that is not one and
 * skips none, and the next transmission lines up with the new clock. The slot's own decodes are not cut (the step
 * puts its end where it belongs) but their DTs are of the old clock: not counted */
#define FTX_SYNC_EDGE_S         1.0f

#define FTX_SYNC_SAMPLES        512     /* decodes kept: a busy FT8 band gives 20 to 30 a slot */

typedef struct {
    int64_t  slot;          /* the slot's number: its start / the slot's length, by the clock of its audio */
    int64_t  clock_set;     /* CLOCK_REALTIME - CLOCK_MONOTONIC as its audio began, ns */
    uint32_t call;          /* the sender, hashed */
    float    dt;            /* s, 0 at FTX_SYNC_START_S into the slot */
} ftx_sync_sample_t;

typedef struct {
    ftx_sync_sample_t s[FTX_SYNC_SAMPLES];
    int               n;
    int               proto;    /* the mode the samples are of, -1 none yet: a change of mode starts over */
} ftx_sync_t;

typedef enum {
    FTX_SYNC_WAIT,          /* too few stations or slots yet, or FT2 */
    FTX_SYNC_SPREAD,        /* the stations disagree */
    FTX_SYNC_ON_TIME,       /* the clock is within FTX_SYNC_STEP_MIN_S */
    FTX_SYNC_TOO_FAR,       /* the decodes put it further off than FTX_SYNC_STEP_MAX_S */
    FTX_SYNC_STEP,          /* move the clock by step */
} ftx_sync_rc_t;

typedef struct {
    float median_dt;        /* the stations' median DT, s */
    float spread;           /* their median distance from it, s */
    float step;             /* what the clock should move by, s: FTX_SYNC_RX_DT_S - median_dt (+ forward) */
    int   stations, slots, decodes;
} ftx_sync_result_t;

/* Whether the mode's decodes count: FT8 and FT4 (cfg_ft8_protocol's values) */
bool ftx_sync_proto(int proto);

void ftx_sync_reset(ftx_sync_t *s);

/* The DT of a signal time_sec into the decoder's audio of proto, that audio begun at start_ns (CLOCK_REALTIME): from
 * FTX_SYNC_START_S into its slot (the nearest slot start), the decoder's lag taken off */
float ftx_sync_dt(int proto, int64_t start_ns, float time_sec);

/* A decode of proto: the decoder's audio began at start_ns (CLOCK_REALTIME, 0 not known) and start_mono_ns
 * (CLOCK_MONOTONIC, the same moment), the signal time_sec into it, sent by call; now_* are the clocks now. False when
 * it does not count: FT2, no call, no stamp, the clock set since its audio began, a DT past FTX_SYNC_DT_MAX_S */
bool ftx_sync_add(ftx_sync_t *s, int proto, int64_t start_ns, int64_t start_mono_ns, float time_sec, const char *call,
                  int64_t now_ns, int64_t now_mono_ns);

/* What the decodes of the last FTX_SYNC_WINDOW_S say, by the clocks now; drops the samples too old or of a clock set
 * since. r (may be NULL) gets the counts, and from FTX_SYNC_SPREAD on the rest */
ftx_sync_rc_t ftx_sync_check(ftx_sync_t *s, int proto, int64_t now_ns, int64_t now_mono_ns, ftx_sync_result_t *r);

/* Whether the clock may move by step (s) now, sec into a slot of proto: the moment of FTX_SYNC_EDGE_S */
bool ftx_sync_moment(int proto, float sec, float step);

#ifdef __cplusplus
}
#endif
