/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later AND BSD-2-Clause
 *
 *  FreeDV RADE V1: reading the callsign FreeDV's own program sends in the End-of-Over. The form is
 *  in freedv_text.h.
 *
 *  A C port of the receiving half of FreeDV's RADE text, freedv/freedv-backend at commit
 *  49006b029853061ca4e992f9632a51875aa3ecf1 (12 November 2026), https://github.com/freedv/freedv-backend:
 *    src/pipeline/rade_text.cpp    the frame: CRC-8, the 6-bit characters, the interleaver, the QPSK on the axes
 *    src/pipeline/ldpc_decode.cpp  soft-decision belief propagation (sum-product) over LDPC(112,56), 30 rounds,
 *                                  the channel's values from each symbol's phase and its amplitude
 *    src/pipeline/HRA_56_56.h      the code's parity-check matrix, which is codec2's HRA_56_56 (David Rowe);
 *                                  hra_56_56[] below is its message half, checked against codec2's H_rows
 *  freedv-gui sent the same form from src/pipeline/rade_text.c (on codec2's LDPC code) from December 2024 until
 *  that code moved to freedv-backend in June 2026. What differs from FreeDV's receiver here:
 *  - the noise is estimated from the 56 symbols themselves (the distance to the nearest point) and from the four
 *    filler symbols in floats 112-119, not from floats 112-179: rade_c's demodulator (rade_ofdm_demod_eoo, at the
 *    commit tools/rade/build.sh pins, the one FreeDV builds too) fills eoo_out only up to float 119, and floats
 *    120-179 hold whatever its stack had. On the same soft bits from the modem (on a PC, FreeDV's own code built
 *    beside this one) FreeDV's decoder read 25 of 120 overs at 6 dB SNR, this one 113;
 *  - a bit's own value is capped well under the decoder's messages (CH_MAX), so one wrong at full strength is put
 *    right;
 *  - a frame is taken only with its code's parity checks, its CRC, zeros after the text and characters FreeDV's
 *    sender makes; FreeDV's leaves undefined characters out and takes the rest;
 *  - its shape decides first whether it is FreeDV's form at all (freedv_text.h), so our own form never goes to the
 *    decoder.
 *
 *  FreeDV's files carry this notice:
 *
 *    Copyright (c) 2026, Mooneer Salem
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without modification, are permitted provided that
 *    the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
 *       following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *       following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 *    WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 *    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *    PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *    OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 *    DAMAGE.
 *
 *  The image carries it in /usr/share/x6100/licenses/rade (src/LICENSE-freedv-backend.txt, freedv-backend's LICENSE).
 */

#include "freedv_text.h"

#include <string.h>

#ifdef RADE_ENABLED

#include <math.h>
#include <stdint.h>

#define NSYM        56              /* QPSK symbols, one per bit pair of the code word */
#define NBITS       112             /* LDPC(112,56): 56 message bits, then 56 parity bits */
#define NCHECK      56
#define MAXDEG      5               /* bits in a parity check: three message bits and two parity bits */
#define SHAPE_FROM  56              /* floats 0-55 (our form's) against 56-111 (FreeDV's second half) */
#define FILL_END    120             /* rade_c's demodulator fills eoo_out up to here */
#define INTERLEAVE  37              /* pair k in symbol (37 k) mod 56 */
#define ROUNDS      30              /* FreeDV's */
#define LLR_MAX     30.0            /* the decoder's messages */
#define CH_MAX      10.0            /* a bit's own value, well under LLR_MAX: a bit in one check only (the last parity
                                       bit) that is wrong at full strength is then put right by that check */

/* HRA_56_56: the message bits in each parity check (codec2's H_rows, counted from 0). The parity half is an
   accumulator: check i also takes parity bits i-1 (not check 0) and i, so the sender computes them in turn */
static const uint8_t hra_56_56[NCHECK][3] = {
    { 3, 13, 40}, {32, 45, 54}, {21, 38, 46}, { 1, 26, 43}, { 6, 42, 55}, {11, 21, 45}, {14, 18, 19},
    {40, 44, 54}, { 5, 15, 20}, { 1, 22, 39}, { 7, 30, 36}, { 3, 51, 52}, { 0,  2, 12}, { 6,  7, 31},
    {19, 50, 51}, { 9, 12, 21}, { 1, 30, 31}, { 0, 10, 23}, {27, 30, 43}, { 6, 35, 41}, { 2, 10, 48},
    {24, 27, 37}, {17, 29, 47}, {18, 20, 23}, {16, 37, 55}, {14, 25, 47}, { 7, 41, 49}, {12, 28, 38},
    { 8, 22, 25}, {11, 32, 49}, {38, 44, 53}, { 5, 18, 25}, {16, 27, 36}, {31, 39, 55}, { 2,  9, 13},
    { 5, 22, 26}, { 9, 40, 45}, {17, 24, 43}, { 3, 28, 44}, {33, 35, 42}, { 0, 28, 52}, {13, 48, 51},
    { 8, 37, 39}, { 4, 10, 20}, {14, 29, 50}, {11, 41, 46}, { 4, 15, 29}, {34, 36, 49}, { 4, 48, 50},
    {16, 34, 42}, {15, 17, 26}, {35, 46, 53}, {19, 23, 52}, { 8, 24, 47}, {33, 53, 54}, {32, 33, 34},
};

static double clampd(double v, double lim)
{
    return v > lim ? lim : v < -lim ? -lim : v;
}

/* Belief propagation over the code, the tanh rule, as ldpc_decode.cpp: llr > 0 is a 0. True when a round ends with
   every parity check holding; bits has the decisions of the last round either way */
static bool ldpc_decode(const double llr[NBITS], uint8_t bits[NBITS], int *rounds)
{
    int    var[NCHECK][MAXDEG], deg[NCHECK];
    int    edges[NBITS][3], nedge[NBITS];       /* a bit's edges, as check * MAXDEG + slot */
    double v2c[NCHECK * MAXDEG], c2v[NCHECK * MAXDEG];

    memset(nedge, 0, sizeof(nedge));
    for (int i = 0; i < NCHECK; i++) {
        int d = 0;

        for (int k = 0; k < 3; k++)
            var[i][d++] = hra_56_56[i][k];
        if (i > 0)
            var[i][d++] = NSYM + i - 1;
        var[i][d++] = NSYM + i;
        deg[i] = d;
        for (int k = 0; k < d; k++) {
            int v = var[i][k], e = i * MAXDEG + k;

            edges[v][nedge[v]++] = e;
            v2c[e] = llr[v];
            c2v[e] = 0.0;
        }
    }
    for (int r = 1; r <= ROUNDS; r++) {
        /* check to bit: 2 atanh of the product of tanh(m/2) over the check's other bits */
        for (int i = 0; i < NCHECK; i++) {
            double t[MAXDEG];

            for (int k = 0; k < deg[i]; k++)
                t[k] = tanh(0.5 * v2c[i * MAXDEG + k]);
            for (int k = 0; k < deg[i]; k++) {
                double p = 1.0;

                for (int j = 0; j < deg[i]; j++)
                    if (j != k)
                        p *= t[j];
                c2v[i * MAXDEG + k] = 2.0 * atanh(clampd(p, 1.0 - 1e-12));
            }
        }
        /* bit to check: its own value and what the other checks said; the decision on all of them */
        for (int v = 0; v < NBITS; v++) {
            double sum = llr[v];

            for (int k = 0; k < nedge[v]; k++)
                sum += c2v[edges[v][k]];
            bits[v] = sum < 0.0;
            for (int k = 0; k < nedge[v]; k++)
                v2c[edges[v][k]] = clampd(sum - c2v[edges[v][k]], LLR_MAX);
        }
        bool ok = true;

        for (int i = 0; i < NCHECK && ok; i++) {
            unsigned x = 0;

            for (int k = 0; k < deg[i]; k++)
                x ^= bits[var[i][k]];
            ok = x == 0;
        }
        *rounds = r;
        if (ok)
            return true;
    }
    return false;
}

/* rade_text.cpp's calculateCRC8_: up to the first 0 */
static uint8_t crc8(const uint8_t *c, int n)
{
    uint8_t crc = 0;

    for (int i = 0; i < n && c[i]; i++) {
        crc ^= c[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x1D) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* A character FreeDV's sender makes, or 0 */
static char from_code(uint8_t c)
{
    if (c >= 1 && c <= 9)
        return (char)(c + 37);          /* & ' ( ) * + , - . */
    if (c >= 10 && c <= 19)
        return (char)('0' + c - 10);
    if (c >= 20 && c <= 45)
        return (char)('A' + c - 20);
    if (c == 46)
        return '/';
    return 0;
}

freedv_text_result_t freedv_text_read(const float *eoo, int n, char text[FREEDV_TEXT_MAX + 1],
                                      freedv_text_diag_t *diag)
{
    freedv_text_diag_t dg = {0};
    int                end = n < FILL_END ? n : FILL_END;
    double             head = 0.0, tail = 0.0;

    text[0] = '\0';
    if (diag)
        *diag = dg;
    if (!eoo || n < FREEDV_TEXT_FLOATS)
        return FREEDV_TEXT_OTHER;
    for (int i = 0; i < end; i++)
        if (!isfinite(eoo[i]))
            return FREEDV_TEXT_UNREAD;

    /* The shape: FreeDV's form puts as much into floats 56-111 as into 0-55 (each symbol one point on an axis), ours
       nothing. Without a signal at all it is neither */
    for (int i = 0; i < SHAPE_FROM; i++)
        head += (double)eoo[i] * eoo[i];
    for (int i = SHAPE_FROM; i < FREEDV_TEXT_FLOATS; i++)
        tail += (double)eoo[i] * eoo[i];
    if (head + tail <= 0.0)
        return FREEDV_TEXT_OTHER;
    dg.fill = head > 0.0 ? (float)(tail / head) : 1e9f;
    if (diag)
        *diag = dg;
    if (tail < 0.5 * head)
        return FREEDV_TEXT_OTHER;

    /* Each symbol as a direction and an amplitude, the pairs back in their order */
    double ux[NSYM], uy[NSYM], amp[NSYM], mean = 0.0;

    for (int k = 0; k < NSYM; k++) {
        int s = (INTERLEAVE * k) % NSYM;
        double x = eoo[2 * s], y = eoo[2 * s + 1], a = sqrt(x * x + y * y);

        amp[k] = a;
        ux[k] = a > 0.0 ? x / a : 0.0;
        uy[k] = a > 0.0 ? y / a : 0.0;
        mean += a;
    }
    mean /= NSYM;
    if (!(mean > 0.0))
        return FREEDV_TEXT_UNREAD;

    /* The noise on the unit circle, per component: each symbol's distance to its nearest point, and the filler's to
       (1,0) */
    double ss = 0.0;
    int    cnt = 0;

    for (int k = 0; k < NSYM; k++) {
        if (amp[k] <= 0.0)
            continue;
        double best = fmax(fmax(ux[k], -ux[k]), fmax(uy[k], -uy[k]));

        ss += 2.0 - 2.0 * best;
        cnt += 2;
    }
    for (int s = NSYM; s < end / 2; s++) {
        double x = eoo[2 * s], y = eoo[2 * s + 1], a = sqrt(x * x + y * y);

        if (a > 0.0) {
            ss += 2.0 - 2.0 * x / a;
            cnt += 2;
        }
    }
    double sigma2 = cnt > 1 ? ss / (cnt - 1) : 1.0;

    if (sigma2 < 1e-4)
        sigma2 = 1e-4;

    /* The bits' values (> 0: a 0), weighted by the symbol's amplitude squared, as ldpc_linear_log_map(): the first bit
       of a pair is 1 towards (0,-1) and (-1,0), the second towards (0,1) and (-1,0) */
    double llr[NBITS];

    for (int k = 0; k < NSYM; k++) {
        double w = (amp[k] / mean) * (amp[k] / mean) / sigma2;

        llr[2 * k] = clampd(w * (ux[k] + uy[k]), CH_MAX);
        llr[2 * k + 1] = clampd(w * (ux[k] - uy[k]), CH_MAX);
    }

    uint8_t bits[NBITS];

    dg.converged = ldpc_decode(llr, bits, &dg.rounds);
    if (diag)
        *diag = dg;
    if (!dg.converged)
        return FREEDV_TEXT_UNREAD;

    /* The message: the CRC, then the eight characters, least significant bit first */
    uint8_t crc = 0, code[FREEDV_TEXT_MAX];
    int     len = 0;

    for (int b = 0; b < 8; b++)
        crc |= (uint8_t)(bits[b] << b);
    for (int c = 0; c < FREEDV_TEXT_MAX; c++) {
        code[c] = 0;
        for (int b = 0; b < 6; b++)
            code[c] |= (uint8_t)(bits[8 + 6 * c + b] << b);
    }
    while (len < FREEDV_TEXT_MAX && code[len])
        len++;
    for (int c = len; c < FREEDV_TEXT_MAX; c++)
        if (code[c])
            return FREEDV_TEXT_UNREAD;      /* FreeDV's sender fills after the text with zeros */
    if (crc8(code, len) != crc)
        return FREEDV_TEXT_UNREAD;

    char out[FREEDV_TEXT_MAX + 1];

    for (int c = 0; c < len; c++)
        if (!(out[c] = from_code(code[c])))
            return FREEDV_TEXT_UNREAD;      /* a code its sender never makes */
    out[len] = '\0';
    dg.crc_ok = true;
    if (diag)
        *diag = dg;
    memcpy(text, out, (size_t)len + 1);
    return FREEDV_TEXT_READ;
}

#else   /* RADE_ENABLED */

freedv_text_result_t freedv_text_read(const float *eoo, int n, char text[FREEDV_TEXT_MAX + 1],
                                      freedv_text_diag_t *diag)
{
    (void)eoo; (void)n;
    text[0] = '\0';
    if (diag)
        memset(diag, 0, sizeof(*diag));
    return FREEDV_TEXT_OTHER;
}

#endif
