/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: finds the strongest tone in the filter band with a sliding
 *  FFT and keeps the per-bin noise floor.
 */

#pragma once

#include <array>
#include <complex>

#include "rx_defs.h"

#include <liquid/liquid.h>

namespace cw {

/* The radio's CW peak filter (APF) colours the noise before the decoder sees
 * it. It is a two-pole resonator run at 12.5 kHz on the receive audio; its
 * |H|^2 at each FFT bin lets the finder divide the colour back out. */
constexpr float APF_RATE = 12500.0f;
float apf_gain2(float tone_hz, float q, float hz);

class ToneFinder {
  public:
    ToneFinder();
    ~ToneFinder();
    ToneFinder(const ToneFinder &)            = delete;
    ToneFinder &operator=(const ToneFinder &) = delete;

    /* One sample in. Every FFT_STEP samples (once the FFT is full) a frame is
     * analysed: returns true and fills `out`. lock_hz is where the tone lock
     * sits now (for PeakReport::at_lock). */
    bool push(float x, float lock_hz, PeakReport &out);

    void  set_band(float lo_hz, float hi_hz);
    void  set_apf(bool on, float tone_hz, float q);
    float noise() const { return noise_; }   /* per-bin noise power, < 0 until the first frame */
    float peak_hz() const { return peak_hz_; }

    /* Last FFT frame, non-negative half (for the tests) */
    const std::array<std::complex<float>, FFT_BINS> &spectrum() const { return spec_; }

  private:
    void analyse(float lock_hz, PeakReport &out);

    /* Samples twice over, so the last FFT_LEN are always contiguous */
    std::array<float, 2 * FFT_LEN> ring_{};
    size_t ring_pos_ = 0;
    size_t filled_   = 0;

    std::array<float, FFT_LEN>               hann_;
    std::array<std::complex<float>, FFT_LEN> time_;
    std::array<std::complex<float>, FFT_LEN> freq_;
    std::array<std::complex<float>, FFT_BINS> spec_;
    fftplan plan_ = nullptr;

    bool                         apf_on_ = false;
    std::array<float, FFT_BINS>  apf_g2_;
    std::array<float, FFT_BINS>  pwr_{};
    std::array<float, FFT_BINS>  sorted_{};

    size_t first_ = 0, last_ = 0; /* band, in bins */
    float  noise_    = -1.0f;
    float  peak_hz_  = 0.0f;
    int    last_bin_ = -1;        /* bin of the last strong frame */
    int    steady_   = 0;
};

} // namespace cw
