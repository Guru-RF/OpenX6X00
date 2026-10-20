/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 test audio for tests and tools
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "js8_submode.h"

namespace x6100 {
namespace js8 {

/// A made-up station for make_test_band().
struct TestStation { // see make_test_band()
    std::string call;                     ///< e.g. "K1ABC"
    std::string grid;                     ///< e.g. "FN42"
    std::string text;                     ///< typed as in JS8Call: "K2XYZ SNR?", "CQ CQ CQ FN42", ...
    double      offset_hz;                ///< audio frequency of tone 0
    double      snr_db;                   ///< over 2500 Hz of noise, the way JS8Call reports SNR
    js8_speed_t speed = JS8_SPEED_NORMAL; ///< each station can use its own
};

/// Audio of several stations sending at once, plus Gaussian noise with an RMS
/// of `noise_rms` (1.0 is full scale). Sample 0 starts a slot at every speed,
/// as a 30 s boundary does. A station sends one frame per slot of its speed,
/// in consecutive slots, beginning its speed's start delay into each, as
/// JS8Call does. The length is that of the longest transmission, rounded up
/// to whole 30 s.
std::vector<float> make_test_band(const std::vector<TestStation> &band, int rate, float noise_rms = 0.02f,
                                  std::uint32_t noise_seed = 1);

} // namespace js8
} // namespace x6100
