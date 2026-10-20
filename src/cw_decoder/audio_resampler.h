/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 */

#pragma once

#include <array>
#include <cstddef>

#include "receiver.h" // first: <complex> before liquid.h

#include <liquid/liquid.h>

namespace cw {

// Feeds a Receiver from an audio stream at another rate (the app's DSP
// gives 11025 Hz): arbitrary-rate resampling to Receiver::RATE.
class AudioResampler {
    static constexpr size_t CHUNK = 128;

    msresamp_rrrf             q_;
    std::array<float, CHUNK> out_;

  public:
    explicit AudioResampler(float in_rate) : q_(msresamp_rrrf_create(Receiver::RATE / in_rate, 60.0f)) {}
    ~AudioResampler() { msresamp_rrrf_destroy(q_); }

    AudioResampler(const AudioResampler &)            = delete;
    AudioResampler &operator=(const AudioResampler &) = delete;

    // Only downsampling is supported: CHUNK inputs give at most CHUNK outputs.
    void process(size_t n, float *samples, Receiver &rx) {
        while (n > 0) {
            unsigned int nx = n < CHUNK ? n : CHUNK;
            unsigned int ny = 0;
            msresamp_rrrf_execute(q_, samples, nx, out_.data(), &ny);
            rx.feed(out_.data(), ny);
            samples += nx;
            n -= nx;
        }
    }
};

} // namespace cw
