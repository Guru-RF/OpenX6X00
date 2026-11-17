/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the DMR app's voice decoder, src/dmr_ambe.c over mbelib (third-party/mbelib), built with ASan and
 *  UBSan (Makefile):
 *  - the AMBE+2 silence frame every DMR network sends between words (B9 E8 81 52 61 73 00 2A 6B, 72 bits) in each of
 *    a burst's three places: no FEC error, the same 49 data bits in all three, 480 quiet samples;
 *  - the burst's layout: each of the 216 voice bits reaches exactly one bit of one frame, the 48 sync bits in the
 *    middle none;
 *  - frames made here with valid FEC from chosen data (Golay 24,12 and 23,12 with mbelib's own generator, C1
 *    scrambled from C0's data): the data comes back bit for bit with no error, and with up to three bit errors in a
 *    Golay word they are corrected and counted;
 *  - random bursts (noise, a network's garbage): samples in range, nothing read or written out of bounds.
 *  mbelib is built with ASan and UBSan but UBSan's array bounds check: it reads log2Ml[L + 1] of the last frame's
 *  parameters, one past that array inside the same struct, weighted by zero (third-party/mbelib/UPSTREAM.md); its
 *  files stay as upstream has them. ASan still sees any read outside the struct. Our code has every check on.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dmr_ambe.h"
#include "mbelib.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const int golayGenerator[12];    /* mbelib's (ecc_const.h, defined in ecc.c) */

static int fails;

#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            fails++;                                                   \
        }                                                              \
    } while (0)

static const uint8_t SILENCE[9] = {0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B};

static void set_bit(uint8_t *b, int n, int v) {
    if (v) b[n >> 3] |= 0x80 >> (n & 7);
    else b[n >> 3] &= ~(0x80 >> (n & 7));
}

static int get_bit(const uint8_t *b, int n) {
    return (b[n >> 3] >> (7 - (n & 7))) & 1;
}

/* The three places of a frame in a burst, as the DMR standard has them: bits 0-71; 72-107 and 156-191; 192-263 */
static int place(int f, int n) {
    return f == 0 ? n : f == 2 ? 192 + n : n < 36 ? 72 + n : 120 + n;
}

static void put_frame(uint8_t *burst, int f, const uint8_t *frame9) {
    for (int n = 0; n < 72; n++) set_bit(burst, place(f, n), get_bit(frame9, n));
}

static void decode_frame(const uint8_t *burst, int f, int *errs2, char ambe_d[49]) {
    char      fr[4][24], err_str[64];
    float     out[160];
    int       errs = 0;
    mbe_parms cur, prev, enh;

    mbe_initMbeParms(&cur, &prev, &enh);
    dmr_ambe_frame(burst, f, fr);
    mbe_processAmbe3600x2450Framef(out, &errs, errs2, err_str, fr, ambe_d, &cur, &prev, &enh, 3);
}

static void test_silence(void) {
    uint8_t burst[DMR_BURST_BYTES] = {0};
    char    d[3][49];
    int     e[3];
    int16_t pcm[DMR_BURST_PCM];

    for (int f = 0; f < 3; f++) put_frame(burst, f, SILENCE);
    memset(burst + 14, 0x5a, 5);    /* inside the sync's bits (108-155): not voice, they must not matter */
    for (int f = 0; f < 3; f++) {
        decode_frame(burst, f, &e[f], d[f]);
        CHECK(e[f] == 0);
    }
    CHECK(!memcmp(d[0], d[1], 49) && !memcmp(d[1], d[2], 49));

    dmr_ambe_t *dec = dmr_ambe_new();

    CHECK(dec != NULL);
    for (int k = 0; k < 50; k++) {
        int peak = 0;

        CHECK(dmr_ambe_burst(dec, burst, pcm) == 0);
        for (int i = 0; i < DMR_BURST_PCM; i++) peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
        CHECK(peak < 1000);     /* silence stays quiet, whatever the level control does */
    }
    dmr_ambe_free(dec);
}

static void test_layout(void) {
    int hits[264] = {0};

    for (int n = 0; n < 264; n++) {
        uint8_t burst[DMR_BURST_BYTES] = {0};

        set_bit(burst, n, 1);
        for (int f = 0; f < 3; f++) {
            char fr[4][24];

            dmr_ambe_frame(burst, f, fr);
            for (int w = 0; w < 4; w++)
                for (int i = 0; i < 24; i++) hits[n] += fr[w][i];
        }
    }
    for (int n = 0; n < 264; n++) {
        bool sync = n >= 108 && n < 156;

        CHECK(hits[n] == (sync ? 0 : 1));
    }
}

/* ---------- frames with valid FEC, made here ---------- */

static int golay_parity(int data12) {
    int p = 0;

    for (int i = 0; i < 12; i++)
        if (data12 & (0x800 >> i)) p ^= golayGenerator[i];
    return p;
}

/* data[49] -> fr[4][24] as the air carries it (C1 scrambled) */
static void encode_words(const char data[49], char fr[4][24]) {
    int c0 = 0, c1 = 0, p;

    memset(fr, 0, 4 * 24);
    for (int i = 0; i < 12; i++) {
        c0 = (c0 << 1) | data[i];
        c1 = (c1 << 1) | data[12 + i];
        fr[0][23 - i] = data[i];
        fr[1][22 - i] = data[12 + i];
    }
    p = golay_parity(c0);
    for (int i = 0; i < 11; i++) fr[0][11 - i] = (p >> (10 - i)) & 1;
    p = golay_parity(c1);
    for (int i = 0; i < 11; i++) fr[1][10 - i] = (p >> (10 - i)) & 1;
    for (int i = 1; i < 24; i++) fr[0][0] ^= fr[0][i];     /* C0's 24th bit: its parity, which mbelib leaves */
    for (int i = 0; i < 11; i++) fr[2][10 - i] = data[24 + i];
    for (int i = 0; i < 14; i++) fr[3][13 - i] = data[35 + i];

    /* C1 scrambled with the sequence C0's data seeds (mbelib's demodulation is the same XOR) */
    unsigned pr = 16u * (unsigned)c0;

    for (int j = 22; j >= 0; j--) {
        pr = (173u * pr + 13849u) & 0xffffu;
        fr[1][j] ^= (char)(pr >> 15);
    }
}

/* fr -> a burst's frame f, through where dmr_ambe_frame takes each burst bit from (test_layout checked it) */
static void put_words(uint8_t *burst, int f, char fr[4][24]) {
    for (int n = 0; n < 264; n++) {
        uint8_t one[DMR_BURST_BYTES] = {0};
        char    t[4][24];

        set_bit(one, n, 1);
        dmr_ambe_frame(one, f, t);
        for (int w = 0; w < 4; w++)
            for (int i = 0; i < 24; i++)
                if (t[w][i]) set_bit(burst, n, fr[w][i]);
    }
}

static void test_made_frames(void) {
    srand(6100);
    for (int k = 0; k < 40; k++) {
        char    data[49], fr[4][24], got[49];
        uint8_t burst[DMR_BURST_BYTES];
        int     f = k % 3, errs2;

        for (int i = 0; i < 49; i++) data[i] = (char)(rand() & 1);
        for (int i = 0; i < DMR_BURST_BYTES; i++) burst[i] = (uint8_t)rand();
        encode_words(data, fr);
        put_words(burst, f, fr);
        decode_frame(burst, f, &errs2, got);
        CHECK(errs2 == 0);
        CHECK(!memcmp(data, got, 49));

        /* one to three wrong bits in C0's and C1's Golay words, the first a data bit: corrected and counted */
        int nerr = 1 + k % 3;

        fr[0][12 + k % 12] ^= 1;
        fr[1][11 + k % 12] ^= 1;
        if (nerr > 1) fr[0][1 + k % 11] ^= 1, fr[1][k % 11] ^= 1;
        if (nerr > 2) fr[0][1 + (k + 5) % 11] ^= 1, fr[1][(k + 4) % 11] ^= 1;
        put_words(burst, f, fr);
        decode_frame(burst, f, &errs2, got);
        CHECK(!memcmp(data, got, 49));
        CHECK(errs2 >= 1);
    }
}

static void test_random(void) {
    dmr_ambe_t *dec = dmr_ambe_new();
    uint8_t     burst[DMR_BURST_BYTES];
    int16_t     pcm[DMR_BURST_PCM];

    srand(6200);
    for (int k = 0; k < 3000; k++) {
        for (int i = 0; i < DMR_BURST_BYTES; i++) burst[i] = (uint8_t)rand();
        if (k % 100 == 0) dmr_ambe_reset(dec);
        int e = dmr_ambe_burst(dec, burst, pcm);

        CHECK(e >= 0 && e <= 3 * 24);
        for (int i = 0; i < DMR_BURST_PCM; i++) CHECK(pcm[i] >= -32000 && pcm[i] <= 32000);
    }
    dmr_ambe_free(dec);
}

int main(void) {
    test_silence();
    test_layout();
    test_made_frames();
    test_random();
    printf("dmr_ambe_test: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
