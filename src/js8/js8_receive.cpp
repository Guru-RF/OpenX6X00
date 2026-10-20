/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive for C code (the JS8 window)
 */

#include "js8_receive.h"

#include "msg_kind.hpp"
#include "rx_engine.hpp"
#include "rate_conv.hpp"
#include "wav_file.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <numeric>
#include <string>
#include <thread>

namespace js8 = x6100::js8;

struct js8_rx { // what a js8_rx_t points to
    std::string                   call;
    int                           rate = 0;
    js8_rx_cb_t                   user{};
    std::unique_ptr<js8::Receiver> rx;

    // Test mode: a thread feeding a WAV file in real time.
    std::thread       player;
    std::atomic<bool> playing{false};
    std::atomic<bool> stop_playing{false};
};

// 30 s is a whole number of slots at every speed (6, 10, 15 and 30 s).
static constexpr std::int64_t ALL_SLOTS_MS = 30 * 1000;

// Copy with truncation; `dst` always ends up NUL-terminated.
template <std::size_t N> static void put(char (&field)[N], const std::string &value) {
    const std::size_t len = std::min(N - 1, value.size());
    std::memcpy(field, value.data(), len);
    field[len] = '\0';
}

static js8_rx_msg_t to_msg(const js8::RxFrame &in, const std::string &my_call) {
    js8_rx_msg_t out{}; // zeroed: tx and alert are the app's to set
    out.utc = in.utc;
    out.snr = (int16_t)in.snr;
    out.dt  = in.dt;
    // The drift its audio was taken with puts it DT late; take DT off.
    out.drift_ms       = in.capture_drift_ms - (int32_t)std::lround(in.dt * 1000.0f);
    out.freq_hz        = in.freq_hz;
    out.type           = (uint8_t)in.type;
    out.submode        = (uint8_t)in.mode;
    out.low_confidence = in.low_confidence;
    out.checksum       = (int8_t)in.checksum;
    out.partial        = in.partial;
    out.msg_id         = in.msg_id;

    const js8::MessageClass kind = js8::classify(in.text, my_call);
    out.heartbeat  = kind.heartbeat;
    out.snr_report = kind.snr_report;
    out.cq         = kind.cq;
    out.to_me      = kind.to_me;
    out.to_group   = kind.to_group;
    put(out.from, kind.from);
    put(out.to, kind.to);
    put(out.text, in.text);
    return out;
}

// Plays `audio` into the receiver at real-time pace: silence until JS8 time
// `start_ms`, then the file. Silence rather than a pause, so the decoder's
// ring has no hole in it.
static void play(js8_rx_t *self, std::vector<float> audio, std::int64_t start_ms) {
    const auto               began  = std::chrono::steady_clock::now();
    const std::int64_t       wait   = start_ms - js8::wall_ms();
    const std::size_t        step   = (std::size_t)self->rate / 50; // 20 ms at a time
    const std::vector<float> quiet(step, 0.0f);
    const std::size_t        lead_in = wait > 0 ? (std::size_t)(wait * self->rate / 1000) : 0;

    std::size_t sent = 0; // everything fed, silence too
    std::size_t next = 0; // first sample of `audio` not fed yet
    while (!self->stop_playing && next < audio.size()) {
        std::size_t n;
        if (sent < lead_in) {
            n = std::min(step, lead_in - sent);
            self->rx->feed(quiet.data(), n);
        } else {
            n = std::min(step, audio.size() - next);
            self->rx->feed(audio.data() + next, n);
            next += n;
        }
        sent += n;
        // Pace by the total so far, so sleep overshoot never adds up.
        std::this_thread::sleep_until(began + std::chrono::microseconds((long long)(sent * 1e6 / self->rate)));
    }
    self->playing = false;
}

// The engine can throw while starting; then there is no receiver.
static std::unique_ptr<js8::Receiver> start_receiver(const js8::Receiver::Config &config,
                                                     js8::Receiver::Callbacks hooks) try {
    return std::make_unique<js8::Receiver>(config, std::move(hooks));
} catch (...) {
    return nullptr;
}

js8_rx_t *js8_rx_create(int rate, int submode_mask, const char *my_call, const js8_rx_cb_t *callbacks) {
    js8_rx_t *self = new (std::nothrow) js8_rx_t;
    if (self == nullptr) return nullptr;
    if (my_call != nullptr) self->call = my_call;
    self->rate = rate;
    if (callbacks != nullptr) self->user = *callbacks;

    js8::Receiver::Config config;
    config.input_rate = rate;
    config.submodes   = submode_mask;

    js8::Receiver::Callbacks hooks;
    hooks.on_frame = [self](const js8::RxFrame &f) {
        if (!self->user.on_frame) return;
        const js8_rx_msg_t m = to_msg(f, self->call);
        self->user.on_frame(&m, self->user.ctx);
    };
    hooks.on_message = [self](const js8::RxFrame &f) {
        if (!self->user.on_message) return;
        const js8_rx_msg_t m = to_msg(f, self->call);
        self->user.on_message(&m, self->user.ctx);
    };
    hooks.on_cycle_done = [self](std::size_t decoded) {
        if (self->user.on_cycle_done) self->user.on_cycle_done((unsigned)decoded, self->user.ctx);
    };
    // Left empty when unused, so the worker skips it.
    if (self->user.on_audio)
        hooks.on_audio = [self](const float *audio, std::size_t count) {
            self->user.on_audio(audio, (unsigned)count, self->user.ctx);
        };

    self->rx = start_receiver(config, std::move(hooks));
    if (self->rx) return self;
    delete self;
    return nullptr;
}

void js8_rx_feed(js8_rx_t *receiver, const float *audio, unsigned count) {
    // While a WAV plays, it is the only audio.
    if (receiver == nullptr || audio == nullptr || count == 0 || receiver->playing) return;
    receiver->rx->feed(audio, count);
}

float js8_rx_play_wav(js8_rx_t *receiver, const char *file, char *err, unsigned err_size) {
    constexpr float FAILED = -1.0f;
    const auto      failed = [err, err_size](const std::string &why) {
        if (err != nullptr && err_size != 0) {
            std::strncpy(err, why.c_str(), err_size - 1);
            err[err_size - 1] = '\0';
        }
        return FAILED;
    };
    if (receiver == nullptr || file == nullptr) return failed("no receiver");

    js8_rx_stop_wav(receiver);

    js8::WavAudio wav;
    std::string   why;
    if (!js8::read_wav(file, wav, why)) return failed(why);
    if (wav.samples.empty()) return failed("empty WAV");

    std::vector<float> at_rate;
    if (wav.rate != receiver->rate) {
        const int common = std::gcd(wav.rate, receiver->rate);
        const int up     = receiver->rate / common;
        const int down   = wav.rate / common;
        // Ratios this awkward would need an enormous filter.
        if (up > 2000 || down > 2000) return failed("unsupported sample rate " + std::to_string(wav.rate));
        js8::RationalResampler(up, down).process(wav.samples.data(), wav.samples.size(), at_rate);
    } else {
        at_rate = std::move(wav.samples);
    }

    // The next 30 s boundary, unless that leaves under half a second to get going.
    const std::int64_t now_ms = js8::wall_ms();
    std::int64_t       start  = now_ms - now_ms % ALL_SLOTS_MS + ALL_SLOTS_MS;
    if (start - now_ms < 500) start += ALL_SLOTS_MS;

    receiver->stop_playing = false;
    receiver->playing      = true;
    receiver->rx->clear_messages();
    receiver->player = std::thread(play, receiver, std::move(at_rate), start);
    return (float)(start - now_ms) / 1000.0f;
}

void js8_rx_stop_wav(js8_rx_t *receiver) {
    if (receiver == nullptr) return;
    receiver->stop_playing = true;
    if (receiver->player.joinable()) receiver->player.join();
    receiver->playing = false;
}

bool js8_rx_wav_active(js8_rx_t *receiver) { return receiver != nullptr && receiver->playing; }

void js8_rx_clear(js8_rx_t *receiver) {
    if (receiver != nullptr) receiver->rx->clear_messages();
}

void js8_rx_set_submodes(js8_rx_t *receiver, int submode_mask) {
    if (receiver != nullptr) receiver->rx->set_submodes(submode_mask);
}

void js8_rx_set_decode_range(js8_rx_t *receiver, int from_hz, int to_hz) {
    if (receiver != nullptr) receiver->rx->set_decode_range(from_hz, to_hz);
}

void js8_rx_set_qso_offset(js8_rx_t *receiver, int hz) {
    if (receiver != nullptr) receiver->rx->set_qso_offset(hz);
}

void js8_rx_destroy(js8_rx_t *receiver) {
    if (receiver == nullptr) return;
    js8_rx_stop_wav(receiver);
    receiver->rx.reset();
    delete receiver;
}

int64_t js8_wall_ms(void) { return js8::wall_ms(); }
void js8_set_drift_ms(int64_t drift) { js8::set_drift_ms(drift); }
int64_t js8_drift_ms(void) { return js8::drift_ms(); }
