/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive, sample rate conversion
 */

#pragma once

#include <cstddef>
#include <vector>

namespace x6100 {
namespace js8 {

/// Changes the sample rate by up / down (both integers) with a polyphase FIR.
/// The radio gives the JS8 window 11025 Hz audio and the decoder wants
/// 12000 Hz, a ratio of 160/147. The filter is a Kaiser-windowed sinc that
/// ends below half of the lower of the two rates, so nothing above it can
/// alias into the output.
class RationalResampler final {
  public:
    RationalResampler(int up, int down, int taps_per_phase = 24, double stopband_db = 80.0);

    /// Resample `count` input samples and append the result to `out`. State
    /// carries over, so audio can arrive in pieces of any size.
    void process(const float *in, std::size_t count, std::vector<float> &out);

    /// Forget all past input, as if just built.
    void reset() {
        tail_.assign(width_ - 1, 0.0f);
        position_ = 0;
    }

    int interp() const { return up_; }
    int decim() const { return down_; }

  private:
    int                up_;
    int                down_;
    int                width_;     // taps in each phase
    std::vector<float> filters_;   // up_ rows of width_ taps, each reversed to run oldest sample first
    std::vector<float> tail_;      // the last width_ - 1 input samples
    int                position_ = 0; // where the next output falls, in units of 1/up_ input sample
};

} // namespace js8
} // namespace x6100
