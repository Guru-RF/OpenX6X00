/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#pragma once

/* The arithmetic behind the panorama and the S-meter, with no radio or LVGL in it,
 * so the tests can run it on the PC */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

/* dB to power and back without powf and log10f, for the loops that convert every bin of every frame: the IQ
 * spectrum and waterfall of the X6100, the base's spectrum averaged in power on the X6200, some 30 000 to 100 000
 * calls a second. Each takes the float apart (exponent and mantissa) and works out the rest with a short polynomial,
 * with no loop and no table:
 *   dsp_db_to_pow(db): 10^(db / 10), within 7e-6 of it (0.00003 dB); db past +-376 dB is taken as +-376 dB
 *   dsp_pow_to_db(p):  10 log10(p), within 8e-5 dB; p under 1.2e-38 (0 too) reads -379 dB, where log10f gave -inf
 * The polynomials are Chebyshev fits of 2^f on -0.5 .. 0.5 (degree 4) and of log2(1 + u) on sqrt(0.5) - 1 ..
 * sqrt(2) - 1 (degree 5). tools/dsp_math checks them against powf and log10f, and that the waterfall's colours,
 * the spectrum's pixels and the noise level come out as with those. The rounding below needs IEEE arithmetic as
 * written: with -ffast-math the compiler may take (x + c) - c for x */
static inline float dsp_db_to_pow(float db) {
    float x = std::min(std::max(db * 0.33219280948873623f, -125.0f), 125.0f);   /* log2(10) / 10 */
    float r = (x + 12582912.0f) - 12582912.0f;      /* x rounded to the nearest whole number (1.5 * 2^23) */
    float f = x - r;
    float p = 0.009666368515387469f;

    p = p * f + 0.055921975842256264f;
    p = p * f + 0.24022349038020335f;
    p = p * f + 0.693121045203427f;
    p = p * f + 1.0f;                               /* 2^f, 0.71 .. 1.41 */

    int32_t bits;

    std::memcpy(&bits, &p, 4);
    bits += (int32_t)r << 23;                       /* times 2^r: added to the exponent */
    std::memcpy(&p, &bits, 4);
    return p;
}

static inline float dsp_pow_to_db(float p) {
    int32_t bits;

    p = std::max(p, 1.1754944e-38f);                /* the smallest normal float: a denormal's exponent is not one */
    std::memcpy(&bits, &p, 4);

    int32_t e = (bits >> 23) - 127;
    int32_t m = (bits & 0x7fffff) | 0x3f800000;     /* the mantissa, 1 .. 2 */
    float   f;

    std::memcpy(&f, &m, 4);
    if (f > 1.41421356f) {                          /* to 0.71 .. 1.41, where the fit is made */
        f *= 0.5f;
        e++;
    }
    f -= 1.0f;

    float l = 0.24327277823433333f;

    l = l * f - 0.3936896830253101f;
    l = l * f + 0.48822569555232936f;
    l = l * f - 0.7202141342183704f;
    l = l * f + 1.442517608220981f;
    l = l * f - 6.308052795937077e-06f;             /* log2 of the mantissa */
    return 3.0102999566398120f * ((float)e + l);    /* 10 log10(2) */
}

/* Four-term Blackman-Harris (periodic form, -92 dB sidelobes), scaled so that the
 * squares add up to 1: white noise then reads its variance in every bin, whatever
 * the FFT length */
static inline void dsp_window_bh4(float *w, size_t n) {
    double energy = 0.0;

    for (size_t i = 0; i < n; i++) {
        double x = 2.0 * M_PI * i / n;

        w[i] = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2.0 * x) - 0.01168 * cos(3.0 * x);
        energy += (double)w[i] * w[i];
    }

    float g = 1.0 / sqrt(energy);

    for (size_t i = 0; i < n; i++)
        w[i] *= g;
}

/* Equivalent noise bandwidth of a window, in bins */
static inline float dsp_window_enbw(const float *w, size_t n) {
    double sum = 0.0, energy = 0.0;

    for (size_t i = 0; i < n; i++) {
        sum += w[i];
        energy += (double)w[i] * w[i];
    }
    return n * energy / (sum * sum);
}

/* Narrowest band the S-meter measures, in bins: a carrier's main lobe spreads over a
 * few bins, a narrower CW filter would cut part of it away */
#define DSP_BAND_MIN_BINS 4.0f

/* Width in bins that dsp_band_power() really adds up for a filter from_hz..to_hz */
static inline float dsp_band_bins(float bin_hz, float from_hz, float to_hz) {
    return std::max(fabsf(to_hz - from_hz) / bin_hz, DSP_BAND_MIN_BINS);
}

/* Power inside from_hz..to_hz of a DC-centred spectrum (mean |X|^2 per bin, taken with a
 * unit-energy window). The edge bins count for the part of their width inside the band,
 * so the reading does not step when the filter edges cross a bin. The result is relative
 * to full scale: a carrier of amplitude a gives a^2, white noise of variance v gives
 * v * bandwidth / sample rate. */
static inline float dsp_band_power(const float *psd, size_t n, float bin_hz, float from_hz, float to_hz) {
    float centre = (from_hz + to_hz) / 2.0f / bin_hz + n / 2.0f;
    float half   = dsp_band_bins(bin_hz, from_hz, to_hz) / 2.0f;
    float lo     = centre - half;     /* bin i covers i - 0.5 .. i + 0.5 */
    float hi     = centre + half;
    int   first  = std::max(0, (int)floorf(lo + 0.5f));
    int   last   = std::min((int)n - 1, (int)floorf(hi + 0.5f));
    double sum   = 0.0;

    for (int i = first; i <= last; i++) {
        float part = std::min(hi, i + 0.5f) - std::max(lo, i - 0.5f);

        if (part > 0.0f)
            sum += part * psd[i];
    }
    return sum / n;
}

/* Noise floor of a DC-centred spectrum, as the level of one bin (linear). The bins are
 * averaged in groups of `group`, then the `pct` percentile of the groups is taken: the
 * carriers can fill most of the span without lifting it. 4% at each end is left out,
 * where the decimator rolls off. */
static inline float dsp_noise_floor(const float *psd, size_t n, size_t group, float pct) {
    size_t             edge = n / 25;
    std::vector<float> groups;

    for (size_t i = edge; i + group <= n - edge; i += group) {
        float sum = 0.0f;

        for (size_t k = 0; k < group; k++)
            sum += psd[i + k];
        groups.push_back(sum / group);
    }
    if (groups.empty())
        return 0.0f;

    auto at = groups.begin() + (size_t)(pct * (groups.size() - 1));

    std::nth_element(groups.begin(), at, groups.end());
    return *at;
}

/* Slow follower of the noise floor (dB) for the waterfall colours. It rises slowly, so a
 * carrier that comes up does not wash the colours out, and falls a bit faster. After
 * reset() the first reading is taken as it is and the next second follows quickly:
 * band and zoom changes settle at once. */
struct DspFloorTrack {
    float db      = NAN;
    float quick_s = 0.0f;

    void reset() {
        db = NAN;
    }

    float update(float est_db, float dt_s) {
        if (std::isnan(db)) {
            db = est_db;
            quick_s = 1.0f;
            return db;
        }

        float tau = quick_s > 0.0f ? 0.15f : (est_db > db ? 4.0f : 2.0f);

        quick_s -= dt_s;
        db += (est_db - db) * (1.0f - expf(-dt_s / tau));
        return db;
    }
};
