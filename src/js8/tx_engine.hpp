/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 transmitter: frame building, audio and slot timing
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "submodes.hpp"

namespace x6100 {
namespace js8 {

/* Every JS8 frame is 79 tones whatever the speed. The lowest offset we
 * transmit at is fixed; the highest depends on the speed's bandwidth
 * (Speed::max_offset_hz()). 20 frames are 5 minutes on the air at Normal. */
constexpr int TX_SYMBOLS = 79;
constexpr int TX_MIN_OFFSET_HZ = 500;
constexpr int TX_MAX_FRAMES = 20;
/* Gaussian bandwidth-time product of the tone shaping. This wide it is
 * almost plain CPFSK for the decoders, with softer edges on the air. */
constexpr double TX_GFSK_BT = 3.0;

struct TxFrame { ///< one frame on the air: its 12 characters, frame type bits and tones
    std::string frame;
    int bits = 0;
    std::array<int, TX_SYMBOLS> tones{};
};

struct TxPlan { ///< the frames of one message plus what the other end will read
    std::vector<TxFrame> frames; ///< in the order they go out
    std::string text;    ///< the message after clean-up: upper case, ends trimmed
    std::string preview; ///< our frames run through our own receiver
    std::string error;   ///< why it can't go out; empty when it can
    js8_speed_t speed = JS8_SPEED_NORMAL;

    bool ok() const { return !frames.empty() && error.empty(); }

    /* Air time in seconds: whole slots for all frames but the last, plus
     * the audio length of the last one. */
    double seconds() const;
};

/* Turns `message` into frames the way desktop JS8Call builds them and
 * decodes them again with our receive path to fill `preview`. Text that
 * names nobody (not directed, not CQ) gets our callsign added so it isn't
 * anonymous. Speeds other than Normal carry free text in the fast data
 * frames and use the modified Costas array, like desktop. */
TxPlan plan_message(const std::string &call, const std::string &grid,
                    const std::string &message, js8_speed_t speed = JS8_SPEED_NORMAL);

/* True for a character a JS8 message may contain. */
bool is_sendable_char(char ch);

/* Audio of one frame at `sample_rate`: 79 GFSK symbols of the speed's
 * symbol time, tones spaced 1 / symbol time apart from `base_hz` up, peak
 * 1.0, both ends faded in and out. */
std::vector<float> synth_frame(const std::array<int, TX_SYMBOLS> &tones, double base_hz, int sample_rate,
                               js8_speed_t speed = JS8_SPEED_NORMAL,
                               double bt = TX_GFSK_BT);

/* When a message queued at `now` (wall clock ms) goes out: the speed's
 * start delay into the current slot if that moment hasn't passed yet,
 * otherwise the same delay into the next slot. */
std::int64_t next_tx_start_ms(std::int64_t now, js8_speed_t speed = JS8_SPEED_NORMAL);

/* Plays a queued message, one frame per slot, on its own thread.
 *
 * `play` is called on that thread for every frame; it keys the radio,
 * plays the audio and comes back when the frame is over (it may take the
 * whole frame time), returning false if it was cut short. stop() only
 * raises a flag, so it can be called from anywhere, `play` included; the
 * host should watch stopping() inside `play` and return soon after. */
class Transmitter final {
public: /* for the C wrapper, the tools and the tests */
    enum class State {
        Idle,
        Waiting, ///< queued, the slot hasn't come yet
        Keying,  ///< a frame is being played
    };

    struct Status { ///< a copy of this goes to on_status at every change
        State state = State::Idle;
        int frame = 0;  ///< which frame (from 1) is on the air or due next
        int frames = 0;
        std::int64_t next_ms = 0;  ///< wall clock ms when that frame starts
        std::string text;
        double offset_hz = 0;
        js8_speed_t speed = JS8_SPEED_NORMAL;
    };

    struct Callbacks { ///< any of them may be left empty
        std::function<bool(const std::vector<float> &samples, const TxFrame &tx, int index, int count)> play;
        std::function<void(const Status &)> on_status; ///< called on the transmitter thread
        std::function<void(const std::string &message, bool completed)> on_done;
    };

    /* Where the time comes from; tests put in their own. wait_until()
     * gives up and returns false as soon as `cancelled` is set. */
    struct Clock { ///< ms since the epoch
        virtual ~Clock() {}
        virtual std::int64_t now_ms() = 0;
        virtual bool wait_until(std::int64_t when_ms, const std::atomic<bool> &cancelled) = 0;
    };

    /* No clock given: JS8 wall time, the one the receiver uses. */
    Transmitter(int rate, Callbacks cb, Clock *clock = nullptr);
    ~Transmitter(); ///< stops, then waits for the thread

    Transmitter(const Transmitter &) = delete;
    Transmitter &operator=(const Transmitter &other) = delete;

    /* Starts sending `plan` at `offset`. Refused (false, reason in
     * `reason`) when the plan isn't ok(), the offset is outside 500 Hz ..
     * max_offset_hz() of the plan's speed, or a message is already going.
     *
     * A non-zero `synth` makes the audio at that tone rather than at
     * `offset`: the radio's TX path wants audio around one fixed tone and
     * reaches the offset by moving the VFO. */
    bool send(const TxPlan &plan, double offset, std::string *reason = nullptr, double synth = 0);

    /* Drops the rest of the message and returns without waiting; busy()
     * stays true until the frame being played has finished. */
    void stop(); // only sets a flag

    bool busy() const { return sending_; }
    bool stopping() const { return abort_; }
    Status status() const; ///< a copy, safe from any thread

private: /* state shared with the worker thread */
    void worker(TxPlan plan, double offset_hz, double synth_hz);
    void publish(const Status &s);
    void collect();

    const int rate_;
    Callbacks callbacks_;
    Clock *clock_;
    std::unique_ptr<Clock> own_clock_; ///< set when no clock was passed in

    std::atomic<bool> sending_{false};
    std::atomic<bool> abort_{false};
    std::thread worker_thread_;

    mutable std::mutex status_lock_;
    Status current_;
};

} // namespace js8
} // namespace x6100
