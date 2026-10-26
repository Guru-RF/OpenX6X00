/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the TCI server's audio formats (tci_server.c): the samples of a TCI audio
 *  frame written and read in the four types TCI knows, and the 24 to 48 kHz interpolation of
 *  the receiver's audio. Nothing in here touches a socket or the radio.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A TCI audio frame's sample type (its header's format word) */
enum { TCI_INT16 = 0, TCI_INT24 = 1, TCI_INT32 = 2, TCI_FLOAT32 = 3 };

/* Bytes of one sample of the type, 0 for a type TCI does not have */
size_t tci_sample_bytes(uint32_t type);

/* The type by its TCI name (int16, int24, int32, float32, any case), -1 for another */
int tci_sample_type(const char *name);

/* The type's TCI name */
const char *tci_sample_type_name(uint32_t type);

/* frames samples (full scale +-1) written as type, each to ch channels side by side, as TCI
 * lays them out (little endian). Beyond full scale clipped. Returns the bytes written:
 * frames * ch * tci_sample_bytes(type) */
size_t tci_encode(const float *in, size_t frames, uint32_t ch, uint32_t type, uint8_t *out);

/* frames frames of ch channels in type: the first channel, as int16 (a float beyond full scale
 * clipped, a NaN silent) */
void tci_decode_first(const uint8_t *in, size_t frames, uint32_t ch, uint32_t type, int16_t *out);

/* Twice the rate: a halfband FIR of 47 taps (Kaiser, 80 dB), 12 multiplies an input sample.
 * Flat within 0.001 dB to 9 kHz, -0.1 dB at 10 kHz; a tone's image at 24 kHz - f below -79 dB
 * up to 9 kHz, below -90 dB up to 4 kHz. The output is 12 input samples (0.5 ms) late. */
#define TCI_UP2_HIST 23

typedef struct {
    float hist[TCI_UP2_HIST];   /* the last input samples; zeros to start */
} tci_up2_t;

/* n samples in, 2n out. out NULL: the samples only go into the history, so a stream nobody
 * takes at 48 kHz for a while starts again where it is */
void tci_up2(tci_up2_t *st, const float *in, size_t n, float *out);

#ifdef __cplusplus
}
#endif
