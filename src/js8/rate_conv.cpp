/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive, sample rate conversion
 */

#include "rate_conv.hpp"

#include <algorithm>
#include <cmath>

namespace x6100 {
namespace js8 {

// I0(x) by its power series; the Kaiser window needs it. The series converges
// fast for the betas used here, so stop once a term no longer matters.
static double modified_bessel_i0(double x) {
    double total = 1.0;
    double term  = 1.0;
    for (int k = 1; k <= 49; ++k) {
        const double half_x_over_k = x / (2.0 * k);
        term *= half_x_over_k * half_x_over_k;
        total += term;
        if (term < 1e-12 * total) break;
    }
    return total;
}

// Kaiser's empirical beta for a given stopband attenuation.
static double beta_for(double attenuation_db) {
    if (attenuation_db > 50.0) return 0.1102 * (attenuation_db - 8.7);
    if (attenuation_db >= 21.0) {
        const double over = attenuation_db - 21.0;
        return 0.5842 * std::pow(over, 0.4) + 0.07886 * over;
    }
    return 0.0; // a plain rectangular window
}

RationalResampler::RationalResampler(int up, int down, int taps_per_phase, double stopband_db)
    : up_(up), down_(down), width_(taps_per_phase) {
    // One long low-pass at up_ times the input rate, later dealt out to the
    // phases. Its edge is half the narrower band (input or output) measured
    // at that rate, moved 8 % lower to leave room for the transition.
    const int    length = up_ * width_;
    const double edge   = 0.5 / std::max(up_, down_) * 0.92;
    const double mid    = (length - 1) / 2.0;
    const double beta   = beta_for(stopband_db);
    const double norm   = modified_bessel_i0(beta);

    std::vector<double> lowpass(length);
    for (int i = 0; i < length; ++i) {
        const double t      = i - mid;
        const double ideal  = t == 0.0 ? 2.0 * edge : std::sin(2.0 * M_PI * edge * t) / (M_PI * t);
        const double x      = t / mid;
        const double window = modified_bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - x * x))) / norm;
        // Inserting up_ - 1 zeros between samples divides the level by up_;
        // the filter gives it back.
        lowpass[i] = ideal * window * up_;
    }

    // Phase p uses every up_-th tap from p. Stored back to front, so the dot
    // product in process() walks the input from oldest to newest.
    filters_.resize((std::size_t)up_ * width_);
    for (int p = 0; p < up_; ++p) {
        float *row = &filters_[(std::size_t)p * width_];
        for (int j = 0; j < width_; ++j) row[width_ - 1 - j] = (float)lowpass[p + j * up_];
    }

    tail_.assign(width_ - 1, 0.0f);
}

void RationalResampler::process(const float *in, std::size_t count, std::vector<float> &out) {
    // Past samples followed by the new ones: the filter window for input i is
    // then simply line[i .. i + width_ - 1].
    std::vector<float> line(tail_);
    line.insert(line.end(), in, in + count);

    out.reserve(out.size() + count * up_ / down_ + 2);
    for (std::size_t i = 0; i < count; ++i) {
        const float *window = &line[i];
        // Every output that falls between this input sample and the next.
        for (; position_ < up_; position_ += down_) {
            const float *taps = &filters_[(std::size_t)position_ * width_];
            float        sum  = 0.0f;
            for (int k = 0; k < width_; ++k) sum += taps[k] * window[k];
            out.push_back(sum);
        }
        position_ -= up_;
    }

    tail_.assign(line.end() - (width_ - 1), line.end());
}

} // namespace js8
} // namespace x6100
