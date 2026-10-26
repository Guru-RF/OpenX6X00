/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the TCI server's audio formats (tci_audio.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "tci_audio.h"

#include <math.h>
#include <string.h>
#include <strings.h>

size_t tci_sample_bytes(uint32_t type) {
    switch (type) {
        case TCI_INT16:   return 2;
        case TCI_INT24:   return 3;
        case TCI_INT32:   return 4;
        case TCI_FLOAT32: return 4;
        default:          return 0;
    }
}

static const char *const type_names[] = {"int16", "int24", "int32", "float32"};

int tci_sample_type(const char *name) {
    for (int i = 0; i < 4; i++)
        if (strcasecmp(name, type_names[i]) == 0)
            return i;
    return -1;
}

const char *tci_sample_type_name(uint32_t type) {
    return type < 4 ? type_names[type] : "int16";
}

size_t tci_encode(const float *in, size_t frames, uint32_t ch, uint32_t type, uint8_t *out) {
    size_t bps = tci_sample_bytes(type);

    for (size_t i = 0; i < frames; i++) {
        float   v = in[i] > 1.0f ? 1.0f : in[i] < -1.0f ? -1.0f : in[i];
        uint8_t s[4];

        switch (type) {
            case TCI_INT16: {
                long q = lrintf(v * 32768.0f);
                q      = q > 32767 ? 32767 : q;
                s[0] = (uint8_t)q;
                s[1] = (uint8_t)(q >> 8);
                break;
            }
            case TCI_INT24: {
                long q = lrint(v * 8388608.0);
                q      = q > 8388607 ? 8388607 : q;
                s[0] = (uint8_t)q;
                s[1] = (uint8_t)(q >> 8);
                s[2] = (uint8_t)(q >> 16);
                break;
            }
            case TCI_INT32: {
                long long q = llrint(v * 2147483648.0);
                q           = q > 2147483647LL ? 2147483647LL : q;
                for (int b = 0; b < 4; b++)
                    s[b] = (uint8_t)(q >> (8 * b));
                break;
            }
            default:        /* float32: the radios and PCs are little endian, as TCI */
                memcpy(s, &v, 4);
                break;
        }
        for (uint32_t c = 0; c < ch; c++) {
            memcpy(out, s, bps);
            out += bps;
        }
    }
    return frames * ch * bps;
}

void tci_decode_first(const uint8_t *in, size_t frames, uint32_t ch, uint32_t type, int16_t *out) {
    size_t step = ch * tci_sample_bytes(type);

    for (size_t i = 0; i < frames; i++) {
        const uint8_t *p = in + i * step;

        switch (type) {
            case TCI_INT16:
                out[i] = (int16_t)(p[0] | p[1] << 8);
                break;
            case TCI_INT24:
                out[i] = (int16_t)(p[1] | p[2] << 8);   /* the top 16 of 24 bits */
                break;
            case TCI_INT32:
                out[i] = (int16_t)(p[2] | p[3] << 8);
                break;
            default: {
                float f;
                memcpy(&f, p, 4);
                if (!(f == f))
                    f = 0.0f;
                out[i] = (int16_t)((f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f) * 32767.0f);
                break;
            }
        }
    }
}

/* The halfband's odd taps, both sides as one: out[2k+1] = sum g[j] (x[k-j] + x[k+1+j]), the even
 * outputs are the input itself. Kaiser window, beta 7.857 (80 dB), 47 taps; scaled so the odd
 * outputs have the gain of the even ones (sum 0.5), which is what keeps a 24 kHz tone out */
static const float g[12] = {
    6.322055543e-01f, -1.993108867e-01f, 1.068433408e-01f, -6.424665848e-02f,
    3.947105701e-02f, -2.378168618e-02f, 1.368033859e-02f, -7.328582142e-03f,
    3.543309537e-03f, -1.469247463e-03f, 4.674352305e-04f, -7.397442458e-05f,
};

void tci_up2(tci_up2_t *st, const float *in, size_t n, float *out) {
    float w[TCI_UP2_HIST + 256];

    while (n) {
        size_t k = n < 256 ? n : 256;

        memcpy(w, st->hist, sizeof(st->hist));
        memcpy(w + TCI_UP2_HIST, in, k * sizeof(float));
        if (out) {
            for (size_t i = 0; i < k; i++) {
                const float *c = w + i + 11;    /* c[0] is 12 samples back from the newest, w[i + 23] */
                float        a = 0.0f;

                for (int j = 0; j < 12; j++)
                    a += g[j] * (c[-j] + c[1 + j]);
                *out++ = c[0];
                *out++ = a;
            }
        }
        memcpy(st->hist, w + k, sizeof(st->hist));
        in += k;
        n  -= k;
    }
}
