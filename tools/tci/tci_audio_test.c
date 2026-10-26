/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the TCI server's audio formats (src/tci_audio.c): the 24 to 48 kHz halfband
 *  swept in frequency (the tone kept, its image at 24 kHz - f gone), the four sample types
 *  written and read back, and what the interpolation costs. `make audio_test` here.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE

#include "tci_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* The amplitude of the f component of x, n samples at fs (Goertzel, whole cycles) */
static double amplitude(const float *x, size_t n, double f, double fs) {
    double w = 2 * M_PI * f / fs, c = 2 * cos(w), s1 = 0, s2 = 0;

    for (size_t i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2        = s1;
        s1        = s0;
    }
    return 2 * hypot(s1 - s2 * cos(w), s2 * sin(w)) / n;
}

/* A tone at 24 kHz through the halfband: its level at 48 kHz, and its image's */
static void sweep(void) {
    enum { N = 24000 };     /* 1 s in: every tone on a 100 Hz grid is whole cycles */
    static float in[N], out[2 * N];
    double       pass = 0, image = -400, image_4k = -400, at10 = 0, at11 = 0;

    printf("halfband 24 -> 48 kHz, a tone at -12 dBFS:\n");
    for (int f = 100; f <= 11900; f += 100) {
        tci_up2_t st = {0};

        for (int i = 0; i < N; i++)
            in[i] = 0.25f * (float)sin(2 * M_PI * f * i / 24000.0);
        tci_up2(&st, in, N, out);
        /* the first 48 outputs hold the start from a zero history: left out, and the rest cut
         * to a multiple of 480 samples, whole cycles of every tone on the 100 Hz grid */
        size_t n   = (2 * N - 48) / 480 * 480;
        double a   = amplitude(out + 48, n, f, 48000) / 0.25;
        double im  = amplitude(out + 48, n, 24000 - f, 48000) / 0.25;
        double adb = 20 * log10(a), idb = 20 * log10(im + 1e-30);

        if (f <= 9000 && fabs(adb) > pass)
            pass = fabs(adb);
        if (f <= 9000 && idb > image)
            image = idb;
        if (f <= 4000 && idb > image_4k)
            image_4k = idb;
        if (f == 10000)
            at10 = adb;
        if (f == 11000)
            at11 = adb;
        if (f == 1000 || f == 3000 || f == 6000 || f == 9000 || f == 10000 || f == 11000)
            printf("  %5d Hz: %+8.4f dB, image at %5d Hz %7.1f dB\n", f, adb, 24000 - f, idb);
    }
    printf("  0.1-9 kHz: flat within %.4f dB, images at most %.1f dB (%.1f dB to 4 kHz)\n", pass, image, image_4k);
    CHECK(pass < 0.01, "passband ripple %.4f dB", pass);
    CHECK(image < -78 && image_4k < -90, "image %.1f dB, %.1f dB to 4 kHz", image, image_4k);
    CHECK(at10 > -0.2 && at11 > -2, "10 kHz %.2f dB, 11 kHz %.2f dB", at10, at11);

    /* sample doubling, for scale: the image a plain repeat leaves */
    for (int i = 0; i < N; i++)
        in[i] = 0.25f * (float)sin(2 * M_PI * 1000 * i / 24000.0);
    for (int i = 0; i < N; i++)
        out[2 * i] = out[2 * i + 1] = in[i];
    printf("  (each sample twice instead: image at 23000 Hz %.1f dB)\n",
           20 * log10(amplitude(out, 2 * N, 23000, 48000) / amplitude(out, 2 * N, 1000, 48000)));
}

/* A stream cut in reads of any length gives what one read gives */
static void blocks(void) {
    enum { N = 5000 };
    static float in[N], one[2 * N], cut[2 * N];
    tci_up2_t    a = {0}, b = {0};
    size_t       done = 0, step = 1;

    for (int i = 0; i < N; i++)
        in[i] = (float)sin(i * 0.37) * 0.8f;
    tci_up2(&a, in, N, one);
    while (done < N) {
        size_t k = done + step > N ? N - done : step;
        tci_up2(&b, in + done, k, cut + 2 * done);
        done += k;
        step = step * 3 % 701 + 1;
    }
    CHECK(!memcmp(one, cut, sizeof(one)), "reads of other lengths differ");
    /* a read only into the history, then one out: as if both had been taken */
    tci_up2_t c = {0};
    tci_up2(&c, in, 300, NULL);
    tci_up2(&c, in + 300, 300, cut);
    CHECK(!memcmp(one + 600, cut, 600 * sizeof(float)), "a read into the history only lost it");
    printf("reads of 1..701 samples and a read into the history only: the same samples\n");
}

/* Written in each type and read back: the first channel, both channels the same */
static void types(void) {
    const float   v[]  = {-1.5f, -1.0f, -0.5f, 0.0f, 0.25f, 32767.0f / 32768.0f, 1.0f, 2.0f};
    const int16_t want[] = {-32768, -32768, -16384, 0, 8192, 32767, 32767, 32767};
    const int     n    = (int)(sizeof(v) / sizeof(v[0]));
    uint8_t       buf[8 * 2 * 4];
    int16_t       back[8];

    for (uint32_t t = TCI_INT16; t <= TCI_FLOAT32; t++) {
        size_t bps   = tci_sample_bytes(t);
        size_t bytes = tci_encode(v, n, 2, t, buf);

        CHECK(bytes == n * 2 * bps, "%s: %zu bytes", tci_sample_type_name(t), bytes);
        for (int i = 0; i < n; i++)
            CHECK(!memcmp(buf + (2 * i) * bps, buf + (2 * i + 1) * bps, bps), "%s: channels differ", tci_sample_type_name(t));
        tci_decode_first(buf, n, 2, t, back);
        for (int i = 0; i < n; i++) {
            /* float32 reads back through x 32767 (as the TX path always did): one off at most */
            int d = abs(back[i] - want[i]);
            CHECK(d <= (t == TCI_FLOAT32 ? 1 : 0), "%s: %g read back as %d, not %d", tci_sample_type_name(t),
                  (double)v[i], back[i], want[i]);
        }
        CHECK(tci_sample_type(tci_sample_type_name(t)) == (int)t, "%s by its name", tci_sample_type_name(t));
    }
    CHECK(tci_sample_type("FLOAT32") == TCI_FLOAT32 && tci_sample_type("int8") < 0 && !tci_sample_bytes(4), "names");
    /* int16 from int16: every value through /32768 and back as it was (the knob's stream) */
    for (int x = -32768; x <= 32767; x++) {
        float   f = x / 32768.0f;
        uint8_t b[2];
        tci_encode(&f, 1, 1, TCI_INT16, b);
        if ((int16_t)(b[0] | b[1] << 8) != x) {
            CHECK(0, "int16 %d came back as %d", x, (int16_t)(b[0] | b[1] << 8));
            break;
        }
    }
    float nan = NAN;
    tci_encode(&nan, 1, 1, TCI_FLOAT32, buf);
    tci_decode_first(buf, 1, 1, TCI_FLOAT32, back);
    CHECK(back[0] == 0, "a NaN read as %d", back[0]);
    printf("int16, int24, int32, float32: written to two channels and read back; int16 bit for bit\n");
}

/* What one second of RX audio costs at 48 kHz: the halfband, and the float32 stereo frames */
static void cost(void) {
    enum { N = 512, ROUNDS = 2000 };     /* a capture read; 2000 of them, 42.7 s of audio */
    static float   in[N], out[2 * N];
    static uint8_t frame[2 * N * 2 * 4];
    tci_up2_t      st = {0};
    struct timespec a, b;
    volatile float sink = 0;

    for (int i = 0; i < N; i++)
        in[i] = (float)sin(i * 0.1) * 0.5f;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int r = 0; r < ROUNDS; r++) {
        tci_up2(&st, in, N, out);
        sink += out[r % (2 * N)];
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double up = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (ROUNDS * N / 24000.0);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int r = 0; r < ROUNDS; r++) {
        tci_encode(out, 2 * N, 2, TCI_FLOAT32, frame);
        sink += frame[r % sizeof(frame)];
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double enc = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (ROUNDS * N / 24000.0);
    printf("cost on this PC, per second of audio: halfband %.0f us (24000 x 12 multiplies, 24 adds), "
           "float32 stereo frames %.0f us\n", up / 1000, enc / 1000);
    (void)sink;
}

int main(void) {
    sweep();
    blocks();
    types();
    cost();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("AUDIO OK\n");
    return 0;
}
