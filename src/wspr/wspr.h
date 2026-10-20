/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - WSPR receive and transmit helpers
 *
 *  RX: the audio (USB, dial on the WSPR frequency) is mixed down from
 *  WSPR_AUDIO_HZ to complex baseband at WSPR_RATE and kept for one 2-minute
 *  cycle; wspr_decode_iq() runs wsprd (third-party/wsprd) on it.
 *  TX: wspr_encode() gives the 162 channel symbols of a type 1 message
 *  (call, 4-character locator, dBm) and wspr_synth() the 4-FSK audio.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WSPR_RATE       375                     /* baseband samples per second */
#define WSPR_SAMPLES    (120 * WSPR_RATE)       /* one cycle */
#define WSPR_AUDIO_HZ   1500                    /* centre of the 200 Hz WSPR window */
#define WSPR_SYMBOLS    162
#define WSPR_SYMBOL_S   (8192.0 / 12000.0)      /* 0.683 s, 110.6 s in all */
#define WSPR_TONE_HZ    (12000.0 / 8192.0)      /* tone spacing, 1.465 Hz */

typedef struct {
    int     snr;
    float   dt;
    int     drift;
    double  freq_hz;    /* RF frequency of the signal */
    char    call[13];
    char    loc[7];
    int     dbm;
} wspr_spot_t;

/* Baseband recorder: audio in, WSPR_RATE I/Q out, at most WSPR_SAMPLES */
typedef struct wspr_bb wspr_bb_t;

wspr_bb_t *wspr_bb_create(uint32_t audio_rate);
void       wspr_bb_destroy(wspr_bb_t *bb);
void       wspr_bb_reset(wspr_bb_t *bb);
void       wspr_bb_feed(wspr_bb_t *bb, const float *samples, unsigned n);
/* Copies the samples so far into i/q (WSPR_SAMPLES each), returns the count */
unsigned   wspr_bb_take(wspr_bb_t *bb, float *i, float *q);

/* Decodes n samples of i/q (arrays of WSPR_SAMPLES, changed in place).
 * Returns the number of spots written to out (strongest first). */
int wspr_decode_iq(float *i, float *q, unsigned n, uint32_t dial_hz, wspr_spot_t *out, int max);

/* Type 1 message. false when the call or the locator can't be sent that way
 * (compound calls with a '/'). */
bool wspr_encode(const char *call, const char *grid, int dbm, uint8_t symbols[WSPR_SYMBOLS]);

/* 4-FSK audio, tone 0 at f0 - 1.5 spacings so the signal is centred on f0.
 * malloc'd, *n samples. */
int16_t *wspr_synth(const uint8_t symbols[WSPR_SYMBOLS], float f0, uint32_t rate, uint32_t *n);

/* Nearest power level WSPR can send (0, 3, 7, 10, ... 60 dBm) */
int wspr_dbm(float watts);

#ifdef __cplusplus
}
#endif
