/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Internet radio's station on the S-meter box (#360). A stream is no signal on the air, so while one
 *  plays the box shows how loud it is instead, on a dB scale of its own and marked Audio (smeter.c), and goes back to
 *  the receiver the moment it stops. These are the parts without LVGL, which tools/audio_meter checks on the PC: the
 *  level of each piece the GUI plays (from the samples it writes anyway, nothing decoded twice), the queue that keeps
 *  a piece's level until the speaker gets to it, the bar's motion, and where the dB of full scale go on the S scale.
 */

#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The scale: -60 dB of full scale at its left end, 0 (the loudest a sample can be) at its right end. A station's
 * music is about -25 to -10 dB on the bar, its peaks near 0 */
#define AUDIO_METER_FLOOR (-60.0f)
#define AUDIO_METER_KNEE (-20.0f) /* at S9: louder is in the meter's red part */

/* The bar falls 20 dB a second, as a broadcast meter does (a peak meter falls 20 dB in about 1.7 s): a beat stands
 * out, and silence takes three seconds to reach the floor. The peak holds 1.5 s, as the S-meter's peak bar, then
 * falls as fast */
#define AUDIO_METER_FALL 20.0f
#define AUDIO_METER_HOLD_MS 1500

/* Levels queued: the playing thread writes pieces of 23 to 46 ms, and the stream holds 800 ms of them before they
 * are heard */
#define AUDIO_METER_RING 64

/* The S scale's places the audio's marks go on (S1, S9 and S9+40 of smeter.h, which checks them) */
#define AUDIO_METER_S1 (-121)
#define AUDIO_METER_S9 (-73)
#define AUDIO_METER_S9_40 (-33)

#ifdef __cplusplus
extern "C" {
#endif

/* A piece's level in dB of full scale, AUDIO_METER_FLOOR at least: its RMS (a full-scale square wave 0, a full-scale
 * sine -3) and its highest sample */
typedef struct {
    float rms;
    float peak;
} audio_level_t;

audio_level_t audio_meter_measure(const int16_t *samples, size_t n);

/* What the box draws, in dB of full scale: the bar is the RMS, up at once and down AUDIO_METER_FALL a second; the
 * peak is the highest sample, held AUDIO_METER_HOLD_MS and then falling as fast, never under the bar */
typedef struct {
    float    bar;
    float    peak;
    uint64_t peak_ms; /* when the peak was set */
    uint64_t last_ms; /* the last step */
} audio_meter_t;

/* Both at the floor, from now on (ms, get_time()) */
void audio_meter_reset(audio_meter_t *m, uint64_t now_ms);

/* The time since the last step taken off, then a level heard now taken in (NULL: none) */
void audio_meter_step(audio_meter_t *m, const audio_level_t *in, uint64_t now_ms);

/* dB of full scale onto the S-meter's dB: -60 to -20 on S1 to S9, -20 to 0 on S9 to S9+40. So every bar of the bars
 * (3 dB to S9, 5 dB over it) is 2.5 dB, every tick of the needle (an S unit, 10 dB over S9) 5 dB, and the S scale's
 * S1, S5, S9, +20 and +40 read -60, -40, -20, -10 and 0 */
float audio_meter_to_s(float dbfs);

/* The levels on their way to the speaker: the playing thread puts each piece's with the time it is heard, the meter
 * takes them as they come due. One thread puts, one other takes (a station plays on one thread at a time); gen is the
 * station's turn, and a level of another turn is dropped at once */
typedef struct {
    uint64_t      due;
    audio_level_t lvl;
    uint32_t      gen;
} audio_meter_entry_t;

typedef struct {
    audio_meter_entry_t e[AUDIO_METER_RING];
    _Atomic uint32_t    head; /* the next put */
    _Atomic uint32_t    tail; /* the next take */
} audio_meter_ring_t;

/* False: the queue is full, the level left out */
bool audio_meter_put(audio_meter_ring_t *r, uint64_t due_ms, audio_level_t lvl, uint32_t gen);

/* The oldest level of turn gen that is due by now_ms; false when none is */
bool audio_meter_take(audio_meter_ring_t *r, uint64_t now_ms, uint32_t gen, audio_level_t *lvl);

#ifdef __cplusplus
}
#endif
