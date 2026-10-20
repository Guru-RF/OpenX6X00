/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive, frames to text
 *
 *  Turns each decoded frame into the text desktop JS8Call would show for it,
 *  trying the frame kinds in desktop's order (DecodedText).
 */

#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace x6100 {
namespace js8 {

/// The frame type bits in every JS8 frame (desktop's Varicode::TransmissionType).
enum FrameBits : int { // also JS8_FRAME_* in js8_receive.h
    FRAME_FIRST = 1 << 0, ///< opens a message
    FRAME_LAST  = 1 << 1, ///< closes it
    FRAME_DATA  = 1 << 2, ///< free text rather than a directed command
};

/// Desktop JS8Call puts decodes of lower quality than this in [brackets].
inline constexpr float LOW_CONFIDENCE_QUALITY = 0.17f;

/// Frame-to-text conversion with a little memory.
/// A station with a compound callsign ("EA8/G4ABC") sends it in a frame of
/// its own; the directed frame after it names only the destination. The
/// callsign is kept for 60 s with the audio offset it came on, so the
/// directed frame from that offset can be shown as coming from it. Keeps
/// state without locking: call from one thread only.
class FrameRenderer final {
  public:
    using Clock = std::chrono::steady_clock; ///< for the 60 s compound memory

    /// Text for one 12-character decoded frame `raw` at audio offset
    /// `offset_hz`. `type_bits` holds the frame's FrameBits; FRAME_DATA is
    /// set in it when the payload, not the frame type, says it's data.
    std::string render(const std::string &raw, int *type_bits, float offset_hz, Clock::time_point now = Clock::now());

  private:
    struct HeardCompound {
        std::string       call;
        float             offset_hz;
        Clock::time_point when;
    };
    std::vector<HeardCompound> heard_;
};

/// 3 to 12 letters, digits and '/', at least one of them a digit.
bool is_callsign_like(const std::string &word);

} // namespace js8
} // namespace x6100
