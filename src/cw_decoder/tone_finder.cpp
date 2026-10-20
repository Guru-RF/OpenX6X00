/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: tone finder.
 */

#include "tone_finder.h"

#include <algorithm>
#include <cstdio>

namespace cw {

/* APF as the radio sets it up: y = b0 (x[n] - x[n-2]) + a1 y[n-1] + a2 y[n-2] at 12.5 kHz */
float apf_gain2(float tone_hz, float q, float hz) {
    const float pi2 = 2.0f * static_cast<float>(M_PI);
    const float fs  = APF_RATE;

    q = std::max(0.1f, q);

    float w0   = pi2 * tone_hz / fs;
    float s    = std::sin(w0 / q * 0.5f);
    float k    = 1.0f / (1.0f + s);
    float b0   = (1.0f - k) * (1.0f + std::log10(q));
    float a1   = 2.0f * k * std::cos(w0);
    float a2   = 1.0f - 2.0f * k;

    float w    = pi2 * hz / fs;
    float num  = 2.0f * b0 * std::sin(w);              /* |b0 (1 - e^-2jw)| */
    float re   = 1.0f - a1 * std::cos(w) - a2 * std::cos(2.0f * w);
    float im   = a1 * std::sin(w) + a2 * std::sin(2.0f * w);
    float den  = re * re + im * im;

    return den > 0.0f ? num * num / den : 0.0f;
}

ToneFinder::ToneFinder() {
    plan_ = fft_create_plan(FFT_LEN, time_.data(), freq_.data(), LIQUID_FFT_FORWARD, 0);
    if (!plan_)
        fprintf(stderr, "cw: no FFT plan\n");

    /* Hann window scaled to unit sum: a tone's bin then reads its amplitude */
    float sum = 0.0f;
    for (size_t i = 0; i < FFT_LEN; i++) {
        hann_[i] = liquid_hann(i, FFT_LEN);
        sum += hann_[i];
    }
    float scale = 1.0f / sum;
    for (float &h : hann_)
        h *= scale;

    apf_g2_.fill(1.0f);
    set_band(400.0f, 1200.0f);
}

ToneFinder::~ToneFinder() {
    if (plan_)
        fft_destroy_plan(plan_);
}

void ToneFinder::set_band(float lo_hz, float hi_hz) {
    first_ = static_cast<size_t>(lo_hz * static_cast<float>(FFT_LEN) / RX_RATE);
    last_  = std::min(FFT_BINS - 1, static_cast<size_t>(hi_hz * static_cast<float>(FFT_LEN) / RX_RATE));
}

void ToneFinder::set_apf(bool on, float tone_hz, float q) {
    apf_on_ = on;
    if (on) {
        for (size_t i = 0; i < FFT_BINS; i++) {
            float g2   = apf_gain2(tone_hz, q, static_cast<float>(i) * BIN_HZ);
            apf_g2_[i] = g2 > 1e-8f ? g2 : 1e-8f;  /* a tone outside the band must not divide by 0 */
        }
    }
    /* The floor was measured in other units: start again */
    noise_ = -1.0f;
}

bool ToneFinder::push(float x, float lock_hz, PeakReport &out) {
    if (!plan_)
        return false;

    ring_[ring_pos_]           = x;
    ring_[ring_pos_ + FFT_LEN] = x;
    ring_pos_ = (ring_pos_ + 1) % FFT_LEN;

    if (++filled_ < FFT_LEN)
        return false;
    filled_ -= FFT_STEP;

    const float *t = &ring_[ring_pos_];   /* oldest sample first */
    for (size_t i = 0; i < FFT_LEN; i++)
        time_[i] = t[i] * hann_[i];
    fft_execute(plan_);
    std::copy_n(freq_.begin(), FFT_BINS, spec_.begin());

    analyse(lock_hz, out);
    return true;
}

void ToneFinder::analyse(float lock_hz, PeakReport &out) {
    for (size_t i = first_; i <= last_; i++)
        pwr_[i] = apf_on_ ? std::norm(spec_[i]) / apf_g2_[i] : std::norm(spec_[i]);

    size_t top = first_;
    for (size_t i = first_ + 1; i <= last_; i++)
        if (pwr_[i] > pwr_[top])
            top = i;
    float top_pwr = pwr_[top];

    /* Noise: the band's 25th percentile, then a slow average (faster down than up,
     * so keying in the band does not lift it) */
    size_t n = last_ - first_ + 1;
    std::copy_n(&pwr_[first_], n, sorted_.begin());
    size_t k = std::min(static_cast<size_t>(0.25f * static_cast<float>(n)), n - 1);
    std::nth_element(sorted_.begin(), sorted_.begin() + k, sorted_.begin() + n);
    float now = sorted_[k];

    if (noise_ < 0.0f)
        noise_ = now;
    else
        noise_ += (now > noise_ ? 0.003f : 0.01f) * (now - noise_);

    /* Peak between bins: parabola through the three dB values */
    float bin = static_cast<float>(top);
    if (top > first_ && top < last_) {
        float l = 10.0f * std::log10(pwr_[top - 1] + 1e-15f);
        float c = 10.0f * std::log10(pwr_[top] + 1e-15f);
        float r = 10.0f * std::log10(pwr_[top + 1] + 1e-15f);
        float d = l - 2.0f * c + r;
        if (d < -1.5f)                 /* a real peak, not a flat top */
            bin = static_cast<float>(top) + 0.5f * (l - r) / d;
    }
    peak_hz_ = bin * RX_RATE / static_cast<float>(FFT_LEN);

    float snr = noise_ > 0.0f ? top_pwr / noise_ : 0.0f;

    if (snr >= from_db(PEAK_STRONG_DB)) {
        bool near = last_bin_ >= 0 && std::abs(static_cast<int>(top) - last_bin_) <= 1;
        steady_   = near ? steady_ + 1 : 1;
        last_bin_ = static_cast<int>(top);
    } else {
        steady_   = 0;
        last_bin_ = -1;
    }

    /* Is the locked station on the air in this frame, or does its bin only hold
     * the skirt of a stronger one? */
    float at_lock = 0.0f;
    long  lb      = std::lround(lock_hz * static_cast<float>(FFT_LEN) / RX_RATE);
    if (lb >= static_cast<long>(first_) && lb <= static_cast<long>(last_) && top_pwr > 0.0f)
        at_lock = pwr_[lb] / top_pwr;

    out = {peak_hz_, snr, steady_, at_lock};
}

} // namespace cw
