/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 WAV files for test mode and tools
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace x6100 {
namespace js8 {

/// Audio read from a WAV file: one channel, float, full scale 1.0.
struct WavAudio { // what read_wav() fills in
    int                rate = 0; ///< Hz
    std::vector<float> samples;  ///< -1 .. 1
};

/// Loads a RIFF/WAVE file of 16-bit PCM. Only the first channel is kept. Any
/// chunks besides "fmt " and "data" are passed over, wherever they are. On
/// failure, false with the reason in `error`.
bool read_wav(const std::string &file, WavAudio &out, std::string &error);

/// Saves mono 16-bit PCM at `rate`; 1.0 is full scale, anything louder is clipped.
bool write_wav(const std::string &file, int rate, const std::vector<float> &samples);

} // namespace js8
} // namespace x6100
