/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive, joining frames into messages
 */

#pragma once

#include "frame_text.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace x6100 {
namespace js8 {

/// A decoded frame after rendering, or a whole message built from frames.
struct RxFrame { // also used for whole messages
    int           utc              = 0;     ///< time of day, HHMMSS
    int           snr              = 0;     ///< dB
    float         dt               = 0.0f;  ///< time offset in seconds
    float         freq_hz          = 0.0f;  ///< audio offset
    std::string   text;                     ///< rendered text
    int           type             = 0;     ///< FrameBits of the frame (of the last one, for a message)
    float         quality          = 0.0f;  ///< decoder quality
    bool          low_confidence   = false; ///< quality under LOW_CONFIDENCE_QUALITY
    int           checksum         = 0;     ///< buffered command check: 1 good (and removed), -1 bad, 0 none
    int           mode             = 0;     ///< desktop submode number, 0 for Normal
    int           capture_drift_ms = 0;     ///< the JS8 drift used while its audio was recorded
    std::int64_t  timestamp_ms     = 0;     ///< JS8 time it arrived
    std::uint32_t msg_id           = 0;     ///< message number; partial texts carry their message's
    bool          partial          = false; ///< text so far of a message whose last frame hasn't come yet

    bool is_first() const { return (type & FRAME_FIRST) != 0; }
    bool is_last() const { return (type & FRAME_LAST) != 0; }
    bool is_data() const { return (type & FRAME_DATA) != 0; }
};

/// Puts multi-frame JS8 transmissions back together.
/// Like desktop JS8Call's message buffers, frames belong together when they
/// have the same speed and offsets within that speed's rxThreshold (10 Hz for
/// Normal and Slow, 16 for Fast, 32 for Turbo). A frame marked first and last
/// is a whole message. When a message's last frame never comes, what was
/// received is sent on after 60 s without a new frame for it, counted from
/// its newest frame so that long messages and Slow's 30 s slots survive.
class MessageAssembler final {
  public:
    using Emit = std::function<void(const RxFrame &message)>;

    static constexpr std::int64_t IDLE_TIMEOUT_MS = 60 * 1000;

    /// `emit` gets each finished message. `partial`, when set, also gets the
    /// text so far after every frame of an unfinished one, so it can be shown
    /// growing as desktop does; the finished message keeps the same msg_id.
    explicit MessageAssembler(Emit emit, Emit partial = nullptr)
        : on_done_(std::move(emit)), on_progress_(std::move(partial)) {}

    void add(const RxFrame &in);

    /// Send on, then forget, messages idle for more than IDLE_TIMEOUT_MS.
    void flush_stale(std::int64_t at_ms);

    /// Forget unfinished messages without sending them.
    void clear() { pending_.clear(); }

  private:
    struct Pending {
        std::vector<RxFrame> got;       // in arrival order
        std::int64_t         newest_ms; // timestamp of the latest
        std::uint32_t        number;    // becomes msg_id
    };
    // Speed (submode number) and rounded offset. A map, so lookups run in key order.
    using Slot = std::pair<int, int>;

    RxFrame finish(const Pending &p) const;
    void    progress(const Pending &p) const;

    Emit                   on_done_;
    Emit                   on_progress_;
    std::map<Slot, Pending> pending_;
    std::uint32_t          last_id_ = 0;
};

/// Catches a frame decoded twice in one slot. Turbo is decoded again every
/// second, as on desktop, so the same frame can come out twice and would
/// otherwise turn into a second message. A repeat is the same speed and the
/// same 12 characters, within rxThreshold in offset, under one slot (less a
/// second of slack for decode timing) after the first.
class DuplicateFilter final {
  public:
    /// True when this frame is a repeat and should be dropped.
    bool seen(int submode, const std::string &raw, float offset_hz, std::int64_t at_ms);

  private:
    struct Seen {
        int          submode;
        std::string  raw;
        float        offset_hz;
        std::int64_t at;
    };
    std::vector<Seen> history_;
};

/// The frames' texts end to end, with a space added only where a directed
/// header meets its payload and neither has one. Public for the tests.
std::string assemble_multipart_text(const std::vector<RxFrame> &parts);

} // namespace js8
} // namespace x6100
