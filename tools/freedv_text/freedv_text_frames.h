/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later AND BSD-2-Clause
 *
 *  For the host tests of tools/freedv_text only: the End-of-Over in FreeDV's form as FreeDV's sender
 *  makes it (rade_text_generate_tx_string() and ldpc_encode() of freedv-backend 49006b02, ported;
 *  FreeDV's notice is in src/freedv_text.c), in ours as freedv_tx.c makes it, and the noise. Included
 *  after src/freedv_text.c, whose code table and CRC it uses.
 */

#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#define EOO_N 180       /* rade_n_eoo_bits() */

/* ---- FreeDV's sender, for the test ---- */

/* convert_callsign_to_ota_string_: 0 = left out */
static uint8_t to_code(char c)
{
    if (c >= 38 && c <= 46)
        return (uint8_t)(c - 37);
    if (c == '/')
        return 46;
    if (c >= '0' && c <= '9')
        return (uint8_t)(c - '0' + 10);
    if (c >= 'A' && c <= 'Z')
        return (uint8_t)(c - 'A' + 20);
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(c - 'a' + 20);
    return 0;
}

/* Any CRC and codes, as freedv-backend's test referenceFrame(): the 56 bits, LDPC(112,56), interleaved, QPSK on
   the axes, the filler */
static void fdv_frame(uint8_t crc, const uint8_t code[8], float eoo[EOO_N])
{
    static const float map[4][2] = {{1, 0}, {0, 1}, {0, -1}, {-1, 0}};
    uint8_t cw[NBITS], par = 0;

    for (int b = 0; b < 8; b++)
        cw[b] = (crc >> b) & 1;
    for (int b = 0; b < 48; b++)
        cw[8 + b] = (code[b / 6] >> (b % 6)) & 1;
    for (int i = 0; i < NCHECK; i++) {
        uint8_t x = 0;

        for (int k = 0; k < 3; k++)
            x ^= cw[hra_56_56[i][k]];
        par ^= x;
        cw[NSYM + i] = par;
    }
    for (int k = 0; k < NSYM; k++) {
        int s = (INTERLEAVE * k) % NSYM, v = cw[2 * k] << 1 | cw[2 * k + 1];

        eoo[2 * s] = map[v][0];
        eoo[2 * s + 1] = map[v][1];
    }
    for (int i = NBITS; i < EOO_N; i++)
        eoo[i] = i % 2 ? 0.0f : 1.0f;
}

/* rade_text_generate_tx_string(): the first 8 characters given, those outside the set left out */
static void fdv_encode(const char *call, float eoo[EOO_N])
{
    uint8_t code[8] = {0};
    int     n = 0, len = (int)strlen(call);

    for (int i = 0; i < len && i < 8; i++) {
        uint8_t c = to_code(call[i]);

        if (c)
            code[n++] = c;
    }
    fdv_frame(crc8(code, 8), code, eoo);
}

/* Our form, as freedv_tx.c's set_callsign() */
static void our_encode(const char *call, float eoo[EOO_N])
{
    int len = (int)strlen(call);

    memset(eoo, 0, sizeof(float) * EOO_N);
    for (int i = 0; i < 8; i++) {
        unsigned char c = i < len ? (unsigned char)call[i] : ' ';

        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        for (int b = 0; b < 7; b++)
            eoo[i * 7 + b] = ((c >> (6 - b)) & 1) ? 1.0f : -1.0f;
    }
}

/* ---- noise ---- */

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t rnd32(void)
{
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return (uint32_t)((rng * 2685821657736338717ull) >> 32);
}

static double uni(void)
{
    return (rnd32() + 0.5) / 4294967296.0;
}

static double gauss(void)
{
    return sqrt(-2.0 * log(uni())) * cos(2.0 * M_PI * uni());
}

/* What rade_c leaves in floats 120-179: anything */
static void junk_tail(float eoo[EOO_N])
{
    for (int i = FILL_END; i < EOO_N; i++)
        eoo[i] = i % 3 == 0 ? NAN : i % 3 == 1 ? INFINITY : 1e30f;
}
