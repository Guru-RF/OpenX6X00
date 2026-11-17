/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - DMR's voice to sound (dmr_ambe.c), with mbelib's AMBE+2 3600x2450 decoder (third-party/mbelib). A
 *  DMR voice burst is 33 bytes, 264 bits: 108 of voice, 48 of sync or embedded signalling, 108 of voice. It carries
 *  three voice frames of 72 bits, 20 ms each; each one comes out as 160 samples at 8 kHz. Decoding only: nothing
 *  here makes a frame to send.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMR_AMBE_RATE   8000
#define DMR_BURST_BYTES 33
#define DMR_FRAME_PCM   160
#define DMR_BURST_PCM   (3 * DMR_FRAME_PCM)     /* 60 ms */

typedef struct dmr_ambe dmr_ambe_t;

/* One decoder a call at a time: mbelib's Golay check keeps its work in static variables (UPSTREAM.md), so one thread
 * decodes */
dmr_ambe_t *dmr_ambe_new(void);
void        dmr_ambe_free(dmr_ambe_t *d);
/* A new call: the last one's voice parameters and the level forgotten */
void        dmr_ambe_reset(dmr_ambe_t *d);
/* A burst's three frames to DMR_BURST_PCM samples at 8 kHz, at a steady level; the bit errors the FEC found in the
 * three. With few errors the frame is what the station sent; with many mbelib repeats the last one or gives silence */
int         dmr_ambe_burst(dmr_ambe_t *d, const uint8_t burst[DMR_BURST_BYTES], int16_t pcm[DMR_BURST_PCM]);

/* Frame f (0, 1 or 2) of a burst as mbelib takes it, its four code words C0..C3 in fr[0..3] (for the tests) */
void dmr_ambe_frame(const uint8_t burst[DMR_BURST_BYTES], int f, char fr[4][24]);

#ifdef __cplusplus
}
#endif
