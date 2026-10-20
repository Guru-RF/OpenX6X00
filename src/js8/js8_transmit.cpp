/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 transmitter, C functions used by the dialog
 */

#include "js8_transmit.h"

#include "tx_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>

using x6100::js8::Transmitter;

struct js8_tx { /* what the C handle points to */
    js8_tx_cb_t host{};
    double synth_hz = 0;
    std::unique_ptr<Transmitter> engine;
    /* Set by js8_tx_destroy() before the Transmitter goes: play() may still ask
     * js8_tx_stopping() while its thread is being joined, and engine is
     * already empty by then. */
    std::atomic<bool> closing{false};
};


/* Copies into a C buffer of `size` bytes, cutting if needed, always ending
 * with a NUL. A NULL or empty buffer is left alone. */
static void put_c_string(char *buf, std::size_t size, const std::string &s) {
    if (buf == nullptr || size == 0) return;
    const std::size_t n = s.size() < size - 1 ? s.size() : size - 1;
    s.copy(buf, n);
    buf[n] = '\0';
}

static std::string or_empty(const char *s) {
    return s ? std::string(s) : std::string();
}

static x6100::js8::TxPlan plan_from_c(const char *my_call, const char *my_grid, const char *text, js8_speed_t speed) {
    return x6100::js8::plan_message(or_empty(my_call), or_empty(my_grid), or_empty(text), speed);
}

static js8_tx_state_t c_state(Transmitter::State s) {
    switch (s) {
    case Transmitter::State::Waiting: return JS8_TX_WAITING;
    case Transmitter::State::Keying: return JS8_TX_KEYING;
    default: return JS8_TX_IDLE;
    }
}

extern "C" void js8_tx_preview(const char *call, const char *grid, const char *message, js8_speed_t speed,
                               js8_tx_preview_t *result) {
    if (result == nullptr) return;
    const x6100::js8::TxPlan plan = plan_from_c(call, grid, message, speed);
    *result = js8_tx_preview_t{};
    result->ok = plan.ok();
    result->frames = (int)plan.frames.size();
    result->seconds = static_cast<float>(plan.seconds());
    put_c_string(result->preview, sizeof result->preview, plan.preview);
    put_c_string(result->error, sizeof result->error, plan.error);
}

extern "C" bool js8_tx_sendable_char(char ch) {
    return x6100::js8::is_sendable_char(ch);
}

extern "C" js8_tx_t *js8_tx_create(int rate, float synth_hz, const js8_tx_cb_t *callbacks) {
    js8_tx_t *h = new (std::nothrow) js8_tx_t;
    if (h == nullptr) return nullptr;
    if (callbacks != nullptr) h->host = *callbacks;
    h->synth_hz = synth_hz;

    Transmitter::Callbacks hooks;
    hooks.play = [h](const std::vector<float> &audio, const x6100::js8::TxFrame &, int index, int count) -> bool {
        if (h->host.play == nullptr) return true;
        /* 80% of full scale, the FT8 synthesiser's level. */
        std::vector<int16_t> samples;
        samples.reserve(audio.size());
        for (float v : audio) samples.push_back((int16_t)std::lrintf(std::clamp(v, -1.0f, 1.0f) * 32767.0f * 0.8f));
        return h->host.play(samples.data(), (unsigned)samples.size(), index, count, h->host.ctx);
    };
    hooks.on_status = [h](const Transmitter::Status &s) {
        if (h->host.on_status == nullptr) return;
        js8_tx_status_t c{};
        c.state = c_state(s.state);
        c.frame = s.frame;
        c.frames = s.frames;
        c.next_ms = s.next_ms;
        c.offset_hz = (float)s.offset_hz;
        c.speed = s.speed;
        put_c_string(c.text, sizeof c.text, s.text);
        h->host.on_status(&c, h->host.ctx);
    };
    hooks.on_done = [h](const std::string &text, bool completed) {
        if (h->host.on_done != nullptr) h->host.on_done(text.c_str(), completed, h->host.ctx);
    };

    js8_tx_t *made = nullptr;
    try { /* only bad_alloc can come out of here */
        h->engine.reset(new Transmitter(rate, std::move(hooks)));
        made = h;
    } catch (...) { /* out of memory */
        delete h;
    }
    return made;
}

extern "C" bool js8_tx_send(js8_tx_t *tx, const char *call, const char *grid, const char *message, float offset_hz,
                            js8_speed_t speed, char *err, unsigned err_size) {
    if (tx == nullptr) return false;
    std::string reason;
    const bool queued = tx->engine->send(plan_from_c(call, grid, message, speed), offset_hz, &reason, tx->synth_hz);
    if (!queued) put_c_string(err, err_size, reason);
    return queued;
}

extern "C" void js8_tx_stop(js8_tx_t *tx) {
    if (tx != nullptr) tx->engine->stop();
}

extern "C" bool js8_tx_busy(js8_tx_t *tx) {
    return tx != nullptr && (tx->closing || tx->engine->busy());
}

extern "C" bool js8_tx_stopping(js8_tx_t *tx) {
    return tx != nullptr && (tx->closing || tx->engine->stopping());
}

extern "C" void js8_tx_destroy(js8_tx_t *tx) {
    if (tx == nullptr) return;
    tx->closing = true;
    tx->engine.reset(); /* the destructor stops the thread and waits for it */
    delete tx;
}
