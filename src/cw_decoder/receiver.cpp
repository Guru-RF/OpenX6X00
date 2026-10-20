/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver.
 */

#include "receiver.h"

#include <utility>

namespace cw {

Receiver::Receiver(TextFn text, KeyFn key, StepFn step)
    : text_(std::move(text)), key_fn_(std::move(key)), step_fn_(std::move(step)) {
    set_floor_db(5.0f);
}

void Receiver::set_apf(bool on, float tone_hz, float q) {
    finder_.set_apf(on, tone_hz, q);
    step_noise_ = -1.0f;
    keyer_.restart();
}

void Receiver::feed(const float *x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (finder_.push(x[i], lock_.hz(), peak_) && finder_.noise() > 0.0f) {
            step_noise_ = finder_.noise() * NOISE_TO_STEP;
            lock_.set_noise(step_noise_);
            if (lock_.follow(peak_)) {
                timer_.new_tone();
                keyer_.new_tone();
                moved_ = true;
            }
        }

        float power = lock_.push(x[i]);
        if (power >= 0.0f && step_noise_ > 0.0f)
            detector_step(power);
    }
}

void Receiver::detector_step(float power) {
    KeyEdge e = keyer_.step(power, step_noise_);
    Sym     s = timer_.step(e.dir, e.back_ms, e.strong);

    if (s != SYM_NONE)
        text_.put(s);
    if (e.dir != 0 && key_fn_)
        key_fn_(e.dir > 0);

    snr_db_ = 10.0f * std::log10(keyer_.power() / step_noise_ + 1e-12f);
    if (step_fn_)
        step_fn_();
    moved_ = false;
}

} // namespace cw
