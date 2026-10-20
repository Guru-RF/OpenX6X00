/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive pipeline
 */

#include "rx_engine.hpp"

#include "msg_kind.hpp"
#include "js8core/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <span>
#include <variant>

namespace x6100 {
namespace js8 {

constexpr int DECODER_RATE = 12000;

// The decoder needs unbroken audio. If the worker ever falls this many
// seconds behind, the backlog is thrown away and timing starts afresh.
constexpr double BACKLOG_LIMIT_S = 5.0;

// Interval between comparisons of the sample count with the clock.
constexpr std::int64_t CHECK_EVERY_MS = 2000;

// When the audio is this far behind the clock, some went missing (a stall,
// or no audio fed while transmitting). Silence takes its place: realigning
// alone would leave a minute-old ring that decodes again as if new.
constexpr std::int64_t MISSING_MS = 1000;
constexpr std::int64_t RING_LENGTH_MS = 60 * 1000;

// The engine's scheduler moves through its ring one capture at a time and
// would skip decode windows on a burst of several seconds (after a stall),
// so audio goes in pieces no bigger than a sound card buffer.
constexpr std::size_t PIECE = 4096;

// Time Sync's drift. JS8 time (decode windows, TX slots, all of it) is the
// system clock plus this, as with desktop JS8Call's DriftingDateTime; the
// system clock is left alone.
static std::atomic<std::int64_t> js8_drift{0};

static int checksum_code(Checksum c) {
    if (c == Checksum::Valid) return 1;
    if (c == Checksum::Invalid) return -1;
    return 0;
}

static std::int64_t system_ms() {
    using std::chrono::milliseconds, std::chrono::system_clock;
    return std::chrono::duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::int64_t wall_ms() { return system_ms() + js8_drift.load(std::memory_order_relaxed); }

void set_drift_ms(std::int64_t drift) { js8_drift.store(drift, std::memory_order_relaxed); }

std::int64_t drift_ms() { return js8_drift.load(std::memory_order_relaxed); }

Receiver::Receiver(const Config &settings, Callbacks handlers)
    : cfg_(settings),
      out_(std::move(handlers)),
      to_12k_(DECODER_RATE / std::gcd(DECODER_RATE, settings.input_rate),
              settings.input_rate / std::gcd(DECODER_RATE, settings.input_rate)),
      messages_(
          [this](const RxFrame &whole) {
              if (!out_.on_message) return;
              RxFrame msg  = whole;
              msg.checksum = checksum_code(verify_command_checksum(msg.text));
              out_.on_message(msg);
          },
          [this](const RxFrame &so_far) {
              // Unchecked: the checksum only arrives with the last frame.
              if (out_.on_message) out_.on_message(so_far);
          }) {
    js8core::EngineConfig setup;
    setup.sample_rate_hz = DECODER_RATE;
    // Every speed gets a slot schedule; set_submodes() decides which decode.
    setup.submodes         = SUBMODE_NORMAL | SUBMODE_FAST | SUBMODE_TURBO | SUBMODE_SLOW;
    setup.spectrum_enabled = false; // the window draws its own waterfall

    js8core::EngineCallbacks hooks;
    hooks.on_event = [this](const js8core::events::Variant &event) {
        if (const auto *done = std::get_if<js8core::events::DecodeFinished>(&event)) {
            if (out_.on_cycle_done) out_.on_cycle_done(done->decoded);
            return;
        }
        const auto *d = std::get_if<js8core::events::Decoded>(&event);
        if (!d) return;
        {
            std::lock_guard<std::mutex> hold(messages_lock_);
            if (repeats_.seen(d->mode, d->data, d->frequency, wall_ms())) return;
        }

        RxFrame f;
        f.type             = d->type;
        f.text             = to_text_.render(d->data, &f.type, d->frequency);
        f.utc              = d->utc;
        f.snr              = d->snr;
        f.dt               = d->xdt;
        f.freq_hz          = d->frequency;
        f.quality          = d->quality;
        f.low_confidence   = d->quality < LOW_CONFIDENCE_QUALITY;
        f.mode             = d->mode;
        f.capture_drift_ms = d->capture_drift_ms;
        f.timestamp_ms     = wall_ms();
        if (out_.on_frame) out_.on_frame(f);

        std::lock_guard<std::mutex> hold(messages_lock_);
        messages_.add(f);
    };
    if (out_.on_log) {
        hooks.on_log   = [this](js8core::LogLevel, std::string_view line) { out_.on_log(std::string(line)); };
        hooks.on_error = [this](std::string_view line) { out_.on_log("error: " + std::string(line)); };
    }

    decoder_ = js8core::make_engine(setup, std::move(hooks), {});
    decoder_->set_submodes(cfg_.submodes);
    // Time Sync may have set a drift before this window was opened.
    drift_given_ = drift_ms();
    if (drift_given_ != 0) decoder_->set_time_drift_ms(drift_given_);
    decoder_->start();

    thread_ = std::thread(&Receiver::run, this);
}

Receiver::~Receiver() noexcept {
    {
        std::lock_guard<std::mutex> hold(queue_lock_);
        quit_ = true;
    }
    queue_cv_.notify_one();
    if (thread_.joinable()) thread_.join();

    // Stopping joins the decode thread, after which nothing calls back.
    decoder_->stop();
    decoder_.reset();
}

void Receiver::set_submodes(int mask) { decoder_->set_submodes(mask); }

void Receiver::set_decode_range(int from_hz, int to_hz) { decoder_->set_decode_range(from_hz, to_hz); }

void Receiver::set_qso_offset(int hz) { decoder_->set_qso_offset(hz); }

void Receiver::feed(const float *samples, std::size_t count) {
    {
        std::lock_guard<std::mutex> hold(queue_lock_);
        queue_.insert(queue_.end(), samples, samples + count);
    }
    queue_cv_.notify_one();
}

void Receiver::run() {
    std::vector<float> batch;
    std::vector<float> resampled;

    while (true) {
        {
            std::unique_lock<std::mutex> hold(queue_lock_);
            // Wake at least every 250 ms so idle messages still time out.
            queue_cv_.wait_for(hold, std::chrono::milliseconds(250), [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            batch.swap(queue_); // queue_ gets the empty buffer, keeping its capacity
        }

        if (batch.size() > BACKLOG_LIMIT_S * cfg_.input_rate) {
            // Far behind: this audio is too old to be any use.
            batch.clear();
            to_12k_.reset();
            synced_ = false;
        }

        if (!batch.empty()) {
            if (out_.on_audio) out_.on_audio(batch.data(), batch.size());
            resampled.clear();
            to_12k_.process(batch.data(), batch.size(), resampled);
            batch.clear();
            to_engine(resampled);
        }

        std::lock_guard<std::mutex> hold(messages_lock_);
        messages_.flush_stale(wall_ms());
    }
}

void Receiver::to_engine(const std::vector<float> &audio) {
    if (audio.empty()) return;

    track_clock(audio.size());

    std::vector<std::int16_t> pcm;
    pcm.reserve(audio.size());
    for (float x : audio) pcm.push_back((std::int16_t)std::lrintf(32767.0f * std::clamp(x, -1.0f, 1.0f)));
    send(pcm.data(), pcm.size());
}

void Receiver::send(const std::int16_t *samples, std::size_t count) {
    for (std::size_t done = 0; done < count; done += PIECE) {
        const std::size_t n = std::min(PIECE, count - done);

        js8core::AudioInputBuffer piece;
        piece.data        = std::as_bytes(std::span<const std::int16_t>(samples + done, n));
        piece.format      = {DECODER_RATE, 1, js8core::SampleType::Int16};
        piece.captured_at = std::chrono::steady_clock::now();
        decoder_->submit_capture(piece);
    }
}

void Receiver::track_clock(std::size_t count) {
    // Time Sync changed the drift: the engine moves its ring to the new time
    // on its next buffer, and counting starts over here so the jump doesn't
    // look like lost audio or a clock error.
    if (const std::int64_t drift = drift_ms(); drift != drift_given_) {
        drift_given_ = drift;
        decoder_->set_time_drift_ms(drift);
        synced_  = true;
        sync_ms_ = wall_ms();
        counted_ = count;
        return;
    }

    const std::int64_t clock_ms = wall_ms();

    // First audio, or after dropping a backlog: line up on this buffer.
    if (!synced_) {
        decoder_->request_realign();
        synced_  = true;
        sync_ms_ = clock_ms;
        counted_ = count;
        return;
    }

    counted_ += count;
    if (cfg_.realign_threshold_ms <= 0) return;

    const std::int64_t passed_ms = clock_ms - sync_ms_;
    if (passed_ms < CHECK_EVERY_MS) return;

    // Audio comes in bursts; measure to the end of this one.
    const std::int64_t heard_ms = (std::int64_t)(counted_ * 1000 / DECODER_RATE);
    const std::int64_t late_ms  = passed_ms - heard_ms;

    if (late_ms >= MISSING_MS) {
        // Silence where the audio is missing, ahead of this buffer; never
        // more than the ring holds.
        const std::vector<std::int16_t> silence((std::size_t)(std::min(late_ms, RING_LENGTH_MS) * DECODER_RATE / 1000),
                                                0);
        send(silence.data(), silence.size());
        if (out_.on_log)
            out_.on_log("gap: " + std::to_string(late_ms) + " ms of audio missing, filled with silence");
        if (late_ms <= RING_LENGTH_MS) {
            counted_ += silence.size();
            return;
        }
        // More than a ring's worth: the ring is now all silence, realign below.
    }

    if (std::llabs(late_ms) > cfg_.realign_threshold_ms) {
        decoder_->request_realign();
        ++realigned_;
        if (out_.on_log) out_.on_log("realign: audio/clock error " + std::to_string(late_ms) + " ms");
        sync_ms_ = clock_ms;
        counted_ = count;
    }
}

} // namespace js8
} // namespace x6100
