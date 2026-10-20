/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - JTTY protocol: source grammar, CRC, tail-biting
 *  convolutional code and its list decoder
 *
 *  JTTY is the keyboard-to-keyboard mode of WSJT-X 3.2 by the WSJT
 *  Development Group (K1JT, K9AN, G4KLA, N9ADG, DL3WDG, W3SZ, KJ5HST,
 *  KD0BTO and others). This is a C port of lib/jtty/ in WSJT-X 3.2.0-rc1
 *  (jtty_source_codec.f90, jtty_mod.f90, tbcc.f90,
 *  jtty_tbcc_list_decoder.f90, jtty_tbcc_decoder.f90) and of the
 *  standard-callsign part of lib/77bit/packjt77.f90. WSJT-X is
 *  GPL-3.0; so is this file.
 *
 *  A frame carries 34 source bits: 32 bits of message grammar, a
 *  reserved bit that is always 0 and the end-of-message bit. A 12-bit CRC
 *  makes 46 bits, a rate-1/2 tail-biting code with K=10 (1167, 1545
 *  octal) makes 92, sent as 46 4-FSK symbols after 13 sync symbols.
 *
 *  A frame is held in a uint64_t, bit 1 of the specification (the first
 *  sent) at bit 33, the EOM bit at bit 0.
 */

#pragma once

#ifndef __cplusplus
#include <complex.h>
#endif
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JTTY_NSYNC      13
#define JTTY_NDATA      46
#define JTTY_NSYM       (JTTY_NSYNC + JTTY_NDATA)
#define JTTY_BAUD       31.25f      /* also the tone spacing */
#define JTTY_MAX_FRAMES 16
#define JTTY_MSG_LEN    80

/* Exchange profile for the text packer (WSJT-X: from the contest type) */
enum { JTTY_PROFILE_UNKNOWN = 0, JTTY_PROFILE_FIELD_DAY = 1, JTTY_PROFILE_RTTY = 2 };

enum {
    JTTY_ATOM_CALL = 0, JTTY_ATOM_EXCH_NUM, JTTY_ATOM_EXCH_LOC, JTTY_ATOM_EXCH_PAIR,
    JTTY_ATOM_EXCH_NUM_TIME, JTTY_ATOM_CONTROL, JTTY_ATOM_GRID4, JTTY_ATOM_TEXT5,
};
enum { JTTY_CALL_CQ = 0, JTTY_CALL_CALL, JTTY_CALL_TU_CQ, JTTY_CALL_CALL_TU, JTTY_CALL_AGN, JTTY_CALL_TU_NOW };

typedef struct {
    int  kind, subtype, role, value, value2;
    char text[14];
} jtty_atom_t;

extern const uint8_t jtty_sync[JTTY_NSYNC];

/* Text to frames: normalised to the JTTY alphabet (upper case, single
 * spaces, '#' for anything else) and packed in the fewest frames. canon,
 * if given (81 bytes), gets the text as it will be received. Returns the
 * number of frames, 0 for empty text, -1 if it does not fit. */
int  jtty_pack_text(const char *text, int profile, uint64_t frames[JTTY_MAX_FRAMES], char *canon);

/* One frame's source word; false if it is not a valid JTTY word. */
bool jtty_unpack_frame(uint64_t frame, jtty_atom_t *atom, bool *eom);

/* Rendering of one atom (81 bytes); TEXT5 keeps its spaces. */
bool jtty_render_atom(const jtty_atom_t *atom, char *out);

/* The 59 tones of a frame: sync, then CRC + convolutional code. */
void jtty_frame_tones(uint64_t frame, uint8_t tones[JTTY_NSYM]);

#ifndef __cplusplus
/* Decode one frame from the 46 data symbols' correlations with the four
 * tones (whole symbol, and the energy of its two halves as a real
 * value), as in WSJT-X: list decoding coherent over 1, 2 and 4 symbols,
 * then on the half-symbol energies. True with a CRC-checked frame. */
bool jtty_decode_frame(const float complex corr[JTTY_NDATA][4],
                       const float complex half[JTTY_NDATA][4], uint64_t *frame);
#endif

#ifdef __cplusplus
}
#endif
