/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 transmitter: frame building, audio and slot timing
 */

#include "tx_engine.hpp"

#include "frame_joiner.hpp"
#include "msg_kind.hpp"
#include "rx_engine.hpp" /* wall_ms() */
#include "frame_text.hpp"

#include "js8core/decoder.hpp"
#include "js8core/protocol/costas.hpp"
#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace x6100 {
namespace js8 {

static const char *const CANT_ENCODE = "JS8 can't encode this message";

/* Upper case with every kind of whitespace turned into a plain space, and
 * no spaces at either end. Spaces in the middle stay as typed: desktop
 * sends them that way and APRS commands depend on it (the addressee is
 * padded to 9 characters). */
static std::string clean_text(const std::string &in) {
    std::string s(in);
    for (char &c : s) c = std::isspace((unsigned char)c) ? ' ' : (char)std::toupper((unsigned char)c);
    const std::size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    const std::size_t last = s.find_last_not_of(' ');
    return s.substr(first, last + 1 - first);
}

/* What a receiving station shows for our frames: they go through the same
 * renderer, assembler and checksum test as decodes off the air. */
static std::string read_back(const std::vector<TxFrame> &frames, int varicode) {
    std::string seen;
    FrameRenderer render;
    MessageAssembler joiner([&seen](const RxFrame &msg) { seen = msg.text; });
    for (const TxFrame &tx : frames) {
        RxFrame got;
        got.type = tx.bits;
        got.freq_hz = 1500;
        got.mode = varicode;
        got.text = render.render(tx.frame, &got.type, got.freq_hz);
        joiner.add(got);
    }
    joiner.flush_stale(INT64_MAX);
    verify_command_checksum(seen);
    return seen;
}

/* JS8 wall time (the receiver's, drift included). Waits in steps of at
 * most 50 ms so a stop request is seen quickly. */
struct JS8Time : Transmitter::Clock {
    std::int64_t now_ms() override { return wall_ms(); }

    bool wait_until(std::int64_t target, const std::atomic<bool> &cancelled) override {
        for (;;) {
            if (cancelled) return false;
            const std::int64_t remaining = target - now_ms();
            if (remaining <= 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(remaining < 50 ? remaining : 50));
        }
    }
};

/* Printable ASCII. Frequent characters have short codes and everything
 * else is escaped in data frames; the tests round-trip each one. That is
 * what lets "@APRSIS CMD ...{01}" through. */
bool is_sendable_char(char ch) {
    return ' ' <= ch && ch <= '~';
}

/* Builds and encodes the frames. Throws when the frame builder or the
 * encoder does, or when a frame comes out shorter than 12 characters. */
static void encode_frames(TxPlan &p, const std::string &call, const std::string &grid, bool add_my_call,
                          const Speed &spd) {
    const auto built = js8core::protocol::varicode::build_message_frames(clean_text(call), clean_text(grid), "",
                                                                         p.text, add_my_call, false, spd.varicode);
    if (built.empty()) {
        p.error = CANT_ENCODE;
        return;
    }

    const auto kind =
        spd.original_costas ? js8core::protocol::CostasType::Original : js8core::protocol::CostasType::Modified;
    const auto &sync = js8core::protocol::costas(kind);
    for (const auto &one : built) {
        /* The encoder reads exactly 12 characters. */
        if (one.first.size() < 12) throw std::runtime_error(CANT_ENCODE);
        TxFrame tf;
        tf.frame.assign(one.first, 0, 12);
        tf.bits = one.second;
        js8core::legacy_encode(tf.bits, sync, tf.frame.c_str(), tf.tones.data());
        p.frames.push_back(std::move(tf));
    }
    p.preview = read_back(p.frames, spd.varicode);
}

TxPlan plan_message(const std::string &call, const std::string &grid, const std::string &message,
                    js8_speed_t which) {
    const Speed &spd = speed(which);
    TxPlan p;
    p.speed = spd.id;
    p.text = clean_text(message);

    if (call.empty()) {
        p.error = "set your callsign first";
        return p;
    }
    if (p.text.empty()) {
        p.error = "nothing to send";
        return p;
    }
    const auto bad = std::find_if(p.text.begin(), p.text.end(), [](char c) { return !is_sendable_char(c); });
    if (bad != p.text.end()) {
        p.error = "JS8 can't send '" + std::string(1, *bad) + "'";
        return p;
    }

    /* Plain text addressed to nobody goes out with our call in front. */
    const MessageClass cls = classify(p.text, call);
    const bool add_my_call = !cls.cq && cls.to.empty();

    /* Frame building throws on text it can't handle, and the encoder on a
     * character outside the JS8 alphabet. Neither should happen after the
     * checks above, but this runs on the GUI thread (and the compose
     * window's counter thread) where an escaped exception would end the
     * program, so the message is refused instead. */
    try { // nothing is kept from a failed attempt
        encode_frames(p, call, grid, add_my_call, spd);
    } catch (const std::exception &ex) {
        p.frames.clear();
        p.error = ex.what();
        return p;
    }

    const int n = (int)p.frames.size();
    if (n > TX_MAX_FRAMES)
        p.error = "too long: " + std::to_string(n) + " frames (max " + std::to_string(TX_MAX_FRAMES) + ")";
    return p;
}

/* GFSK as in src/ft8/gfsk.c: the instantaneous frequency of each symbol is
 * spread over three symbol times by a Gaussian-filtered rectangular pulse,
 * then integrated into phase. Here the BT is a parameter and the samples
 * are float. Every expression below is evaluated in the same order as the
 * FT8 code so the samples come out the same to the last bit. */
std::vector<float> synth_frame(const std::array<int, TX_SYMBOLS> &tones, double base_hz, int sample_rate,
                               js8_speed_t which, double bt) {
    const double gauss_k = 5.336446; /* pi * sqrt(2 / ln 2) */
    const int sps = (int)std::lround(speed(which).symbol_seconds() * sample_rate);
    const int span = 3 * sps;
    const std::size_t len = (std::size_t)TX_SYMBOLS * sps;
    /* Phase step per sample of one tone spacing, which is 1 / symbol time. */
    const double spacing = 2.0 * M_PI / sps;

    std::vector<double> shape(span);
    for (int n = 0; n < span; n++) {
        const double x = n / (double)sps - 1.5;
        shape[n] = (std::erf(gauss_k * bt * (x + 0.5)) - std::erf(gauss_k * bt * (x - 0.5))) / 2.0;
    }

    /* Phase step for every sample, with one extra symbol before and after
     * the frame for the pulse tails. Sample k of the frame is entry k + sps. */
    std::vector<double> step(len + 2 * sps, 2.0 * M_PI * base_hz / sample_rate);
    for (int sym = 0; sym < TX_SYMBOLS; sym++) {
        double *at = &step[(std::size_t)sym * sps];
        for (int n = 0; n < span; n++) at[n] += spacing * tones[sym] * shape[n];
    }
    /* Pretend the first and last tones carry on beyond the frame, so the
     * edges get the same smooth shape as the transitions inside. */
    for (int n = 0; n < 2 * sps; n++) {
        step[n] += spacing * shape[sps + n] * tones[0];
        step[len + n] += spacing * shape[n] * tones[TX_SYMBOLS - 1];
    }

    std::vector<float> audio(len);
    double phase = 0.0;
    for (std::size_t k = 0; k < len; k++) {
        audio[k] = (float)std::sin(phase);
        phase = std::fmod(phase + step[sps + k], 2.0 * M_PI);
    }

    /* Fade in and out over 1/8 of a symbol (raised cosine), like FT8 TX. */
    const int fade = sps / 8;
    for (int n = 0; n < fade; n++) {
        const float g = (float)((1.0 - std::cos(M_PI * n / fade)) / 2.0);
        audio[n] *= g;
        audio[len - 1 - n] *= g;
    }
    return audio;
}

double TxPlan::seconds() const {
    if (frames.empty()) return 0.0;
    const Speed &s = x6100::js8::speed(speed);
    return (double)(frames.size() - 1) * s.period_s + s.frame_seconds();
}

std::int64_t next_tx_start_ms(std::int64_t now, js8_speed_t speed_id) {
    const Speed &spd = speed(speed_id);
    const std::int64_t period = spd.period_ms();
    const std::int64_t slot = now - now % period;
    const bool too_late = now - slot >= spd.start_delay_ms;
    return slot + (too_late ? period : 0) + spd.start_delay_ms;
}

Transmitter::Transmitter(int rate, Callbacks cb, Clock *clock)
    : rate_(rate), callbacks_(std::move(cb)), clock_(clock) {
    /* Each transmitter owns its default clock, so it outlives the thread. */
    if (clock_ == nullptr) {
        own_clock_.reset(new JS8Time);
        clock_ = own_clock_.get();
    }
}

Transmitter::~Transmitter() { stop(); collect(); }

/* Joins the worker of an earlier message, if there is one. */
void Transmitter::collect() {
    if (worker_thread_.joinable()) worker_thread_.join();
}

bool Transmitter::send(const TxPlan &plan, double offset, std::string *reason, double synth) {
    std::string refusal;
    const Speed &spd = speed(plan.speed);
    const int top = spd.max_offset_hz();
    if (!plan.ok()) {
        refusal = plan.error.empty() ? "nothing to send" : plan.error;
    } else if (offset < TX_MIN_OFFSET_HZ || offset > top) {
        refusal = "offset must be " + std::to_string(TX_MIN_OFFSET_HZ) + "-" + std::to_string(top) + " Hz in " +
                  spd.name;
    } else if (sending_.exchange(true)) {
        refusal = "already sending";
    } else {
        collect(); /* sending_ was false, so that thread has finished */
        abort_ = false;
        worker_thread_ = std::thread(&Transmitter::worker, this, plan, offset, synth > 0 ? synth : offset);
        return true;
    }
    if (reason) *reason = refusal;
    return false;
}

/* Just a flag, so any thread may call it, the play callback too. The
 * worker thread is joined later by send() or the destructor. */
void Transmitter::stop() { abort_ = true; }

Transmitter::Status Transmitter::status() const {  /* a copy taken under the lock */
    std::lock_guard<std::mutex> hold(status_lock_);
    return current_;
}

void Transmitter::publish(const Status &s) {
    {
        std::lock_guard<std::mutex> hold(status_lock_);
        current_ = s;
    }
    if (callbacks_.on_status) callbacks_.on_status(s);
}

void Transmitter::worker(TxPlan plan, double offset_hz, double synth_hz) {
    const Speed &spd = speed(plan.speed);
    const int total = (int)plan.frames.size();

    Status shown;
    shown.frames = total;
    shown.text = plan.text;
    shown.offset_hz = offset_hz;
    shown.speed = plan.speed;

    bool all_sent = false;
    std::int64_t slot_at = next_tx_start_ms(clock_->now_ms(), spd.id);

    for (int idx = 0; idx < total; idx++) {
        if (abort_) break;
        /* Frames go in back-to-back slots. If playing the previous one ran
         * past its slot, take the next one that is still ahead. */
        const std::int64_t now = clock_->now_ms();
        if (slot_at < now) slot_at = next_tx_start_ms(now, spd.id);

        /* Make the audio first so the wait ends right on the slot. */
        const std::vector<float> audio = synth_frame(plan.frames[idx].tones, synth_hz, rate_, spd.id);

        shown.state = State::Waiting;
        shown.frame = idx + 1;
        shown.next_ms = slot_at;
        publish(shown);
        if (!clock_->wait_until(slot_at, abort_)) break;

        shown.state = State::Keying;
        publish(shown);
        const bool finished = !callbacks_.play || callbacks_.play(audio, plan.frames[idx], idx, total);
        if (!finished || abort_) break;

        slot_at += spd.period_ms();
        all_sent = idx + 1 == total;
    }

    publish(Status());
    sending_ = false;
    if (callbacks_.on_done) callbacks_.on_done(plan.text, all_sent);
}

} // namespace js8
} // namespace x6100
