/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later AND BSD-2-Clause
 *
 *  FreeDV RADE V1: the callsign FreeDV's own program sends in the End-of-Over, read from the
 *  End-of-Over's soft bits (rade_rx()'s eoo_out). freedv_text.c is a port of FreeDV's receiver of
 *  it; the source and FreeDV's notice are there.
 *
 *  FreeDV's form (freedv-backend's src/pipeline/rade_text.cpp, the same since freedv-gui sent it
 *  first in December 2024), in the 180 floats of rade_tx_set_eoo_bits():
 *    - up to 8 characters of 6 bits: 1-9 are & ' ( ) * + , - . (ASCII 38-46), 10-19 the digits,
 *      20-45 A-Z, 46 '/', and 0 after the last (FreeDV before July 2026 sent '/' as 10, which
 *      reads as '0');
 *    - a CRC-8 over the characters up to the first 0: polynomial 0x1D, starting from 0;
 *    - 56 bits: the CRC, least significant bit first, then each character, least significant bit
 *      first;
 *    - LDPC(112,56), codec2's HRA_56_56: those 56 bits, then 56 parity bits;
 *    - the bits in pairs (2k, 2k+1), each pair one QPSK symbol on the axes: 00 = (1,0),
 *      01 = (0,1), 10 = (0,-1), 11 = (-1,0); pair k goes to symbol (37 k) mod 56, symbol s to
 *      floats 2s (I) and 2s+1 (Q): floats 0-111;
 *    - floats 112-179: (1,0) in every symbol, known filler.
 *  The decoder is FreeDV's kind: soft decisions, belief propagation (sum-product), 30 rounds.
 *
 *  Our own form (freedv_tx.c, as rade_c's rade_v1_text_test.c) is 8 characters of 7 bits, +-1, in
 *  floats 0-55, and zeros after them. So the two tell apart by their shape: FreeDV's fills floats
 *  56-111 as much as floats 0-55, ours leaves them empty.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREEDV_TEXT_MAX     8       /* characters */
#define FREEDV_TEXT_FLOATS  112     /* the soft bits it needs; the four symbols after them help the noise estimate */

typedef enum {
    FREEDV_TEXT_OTHER = 0,  /* not FreeDV's form: floats 56-111 are empty, as our form leaves them */
    FREEDV_TEXT_READ,       /* FreeDV's form, and every check passed: text is what was sent ("": no callsign) */
    FREEDV_TEXT_UNREAD,     /* FreeDV's form by its shape (or noise), and a check failed: nothing to read */
} freedv_text_result_t;

/* What the checks found, for the tests */
typedef struct {
    float fill;             /* the power of floats 56-111 over that of floats 0-55: about 1 for FreeDV's, 0 for ours */
    int   rounds;           /* the decoder's rounds */
    bool  converged;        /* every parity check of the LDPC code holds */
    bool  crc_ok;           /* ... and the CRC-8, the padding after the text and the character set too */
} freedv_text_diag_t;

/* Reads FreeDV's form from n soft bits (rade_rx()'s eoo_out, rade_n_eoo_bits() of them; only the first 120 are
   looked at, which is as far as rade_c's demodulator fills them). text gets FREEDV_TEXT_MAX characters at most and
   its NUL, "" unless FREEDV_TEXT_READ. diag may be NULL. Any thread; no state */
freedv_text_result_t freedv_text_read(const float *eoo, int n, char text[FREEDV_TEXT_MAX + 1],
                                      freedv_text_diag_t *diag);

#ifdef __cplusplus
}
#endif
