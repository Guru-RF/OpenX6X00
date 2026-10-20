/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive pipeline
 */

#pragma once

#include "frame_joiner.hpp"
#include "frame_text.hpp"
#include "rate_conv.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace js8core { class Js8Engine; } // engine.hpp stays out of this header

namespace x6100 {
namespace js8 {

/// Bits for Receiver::Config::submodes, in js8core's SubmodeId order.
enum SubmodeMask : int { // same bits as JS8_SUBMODE_* in js8_receive.h
    SUBMODE_NORMAL = 0x1,
    SUBMODE_FAST   = 0x2,
    SUBMODE_TURBO  = 0x4,
    SUBMODE_SLOW   = 0x8,
};

/// Audio in, decoded frames and messages out.
/// feed() is cheap enough for the audio thread: it only queues samples. A
/// worker thread brings them to 12 kHz and passes them to the js8core engine,
/// which decodes on a thread of its own. Callbacks therefore come from those
/// two threads; anything touching the UI has to hand them over to its thread.
class Receiver final {
  public:
    struct Config { // fixed for the receiver's life
        int input_rate = 11025;          ///< Hz of the audio given to feed()
        int submodes   = SUBMODE_NORMAL; ///< SubmodeMask bits to decode
        /// When the samples received and the time passed differ by more than
        /// this, the decoder's 60 s ring is lined up with the clock again.
        /// 0 turns the check off, for tests that feed faster than real time.
        int realign_threshold_ms = 150; // ms
    };

    struct Callbacks { // each may be left empty
        /// Each frame as decoded and rendered, for the band activity list.
        std::function<void(const RxFrame &frame)> on_frame;
        /// Whole messages once assembled, and, with `partial` set, a message's
        /// text so far after each frame until it's complete.
        std::function<void(const RxFrame &message)> on_message;
        /// End of a decode pass, with the number of distinct decodes.
        std::function<void(std::size_t decodes)> on_cycle_done;
        /// The audio at the input rate as the worker picks it up (waterfall).
        std::function<void(const float *samples, std::size_t count)> on_audio;
        /// Engine diagnostics; a lot of them, so normally left unset.
        std::function<void(const std::string &line)> on_log;
    };

    Receiver(const Config &settings, Callbacks handlers);
    ~Receiver(); ///< stops both threads: no callback runs once it returns

    Receiver(const Receiver &other) = delete;
    Receiver &operator=(const Receiver &other) = delete;

    /// Queue samples (float, -1..1, at Config::input_rate). Any thread.
    void feed(const float *samples, std::size_t count);

    /// Forget messages still being received, e.g. on a band change.
    void clear_messages() {
        std::lock_guard<std::mutex> hold(messages_lock_);
        messages_.clear();
    }

    /// Switch speeds on or off (SubmodeMask), from any thread. Slots are
    /// tracked for all speeds all the time; a speed's decoder is set up the
    /// first time it is switched on.
    void set_submodes(int mask);

    /// Where to look for signals and our own TX offset (decoded first), the
    /// same two things desktop JS8Call takes from its filter and freq().
    void set_decode_range(int from_hz, int to_hz);
    void set_qso_offset(int hz);

    /// Times the ring was lined up with the clock again since the start.
    unsigned realign_count() const { return realigned_.load(); }

  private:
    void run();
    void to_engine(const std::vector<float> &audio);
    void track_clock(std::size_t count);
    void send(const std::int16_t *samples, std::size_t count);

    Config    cfg_;
    Callbacks out_;

    std::unique_ptr<js8core::Js8Engine> decoder_;
    RationalResampler                   to_12k_;

    // Audio from feed() to the worker.
    std::mutex              queue_lock_;
    std::condition_variable queue_cv_;
    std::vector<float>      queue_;
    bool                    quit_ = false;
    std::thread             thread_;

    // Sample count against the clock; the worker alone touches these.
    bool                  synced_      = false;
    std::int64_t          drift_given_ = 0; // last drift handed to the engine
    std::int64_t          sync_ms_     = 0; // JS8 time when counting began
    std::uint64_t         counted_     = 0; // 12 kHz samples since then
    std::atomic<unsigned> realigned_{0};

    // Used on the decode thread; the assembler and duplicate filter also by
    // the worker, so both sit behind messages_lock_.
    FrameRenderer    to_text_;
    std::mutex       messages_lock_;
    DuplicateFilter  repeats_;
    MessageAssembler messages_;
};

std::int64_t wall_ms();                   ///< JS8 time, ms since 1970: system clock plus drift
void         set_drift_ms(std::int64_t drift); ///< set by Time Sync; one for the whole process
std::int64_t drift_ms();                  ///< 0 until Time Sync sets it

} // namespace js8
} // namespace x6100
