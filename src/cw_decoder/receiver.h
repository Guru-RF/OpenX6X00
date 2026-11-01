/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: 4 kHz audio in, text out.
 *
 *  tone finder (FFT) -> tone lock (NCO, PLL) -> key detector -> element
 *  timer -> Morse text
 *
 *  The design (FFT tone search, coherent lock with a PLL, Schmitt detector
 *  following the mark peak, histogram of mark and gap lengths) is the one of
 *  Georgy Dyuldin's receiver in x6100_gui; this is a new implementation of it.
 */

#pragma once

#include <functional>

#include "element_timer.h"
#include "key_detector.h"
#include "morse_text.h"
#include "rx_defs.h"
#include "tone_finder.h"
#include "tone_lock.h"

namespace cw {

class Receiver {
  public:
    using TextFn = MorseText::Out;
    using KeyFn  = std::function<void(bool)>;
    using StepFn = std::function<void()>;

    static constexpr float RATE = RX_RATE;   /* feed() wants this rate */

    /* text: each decoded character, and whether it holds our own keying
     * (own()); key: key down/up; step: after every detector step (8 ms) */
    Receiver(TextFn text, KeyFn key, StepFn step = {});

    void feed(const float *x, size_t n);

    float wpm() const { return timer_.wpm(); }
    float peak_hz() const { return finder_.peak_hz(); }   /* FFT peak of the last frame */
    float lock_hz() const { return lock_.hz(); }          /* the tone being decoded */
    bool  locked() const { return lock_.holding(); }
    float snr_db() const { return snr_db_; }              /* last step, on the locked tone */

    void  set_floor_db(float db) { keyer_.set_floor_db(db); }
    void  set_band(float lo_hz, float hi_hz) { finder_.set_band(lo_hz, hi_hz); }
    /* The radio's peak filter (APF) is on: its colour is taken out of the noise */
    void  set_apf(bool on, float tone_hz, float q);
    /* The VFO moved: the station is df Hz further up in the audio */
    void  follow_vfo(float df) { lock_.shift(df); }
    /* The next n dits and dahs are our own keying (MorseText::own), not
     * learned (ElementTimer::ours) */
    void  own(int n) { text_.own(n); }
    /* Our own keying follows at wpm (the CW trainer's, ElementTimer::expect) */
    void  expect(float wpm) { timer_.expect(wpm); }
    /* The CW trainer is open: down to 5 WPM (ElementTimer::trainer_range) */
    void  trainer_range(bool on) { timer_.trainer_range(on); }

  private:
    friend struct ReceiverPeek;   /* tests only */

    void detector_step(float power);

    ToneFinder   finder_;
    ToneLock     lock_;
    KeyDetector  keyer_;
    ElementTimer timer_;
    MorseText    text_;
    KeyFn        key_fn_;
    StepFn       step_fn_;

    float step_noise_ = -1.0f;   /* noise of one integrator step, < 0 = not known */
    float snr_db_     = 0.0f;
    bool  moved_      = false;   /* the lock moved since the last step (for the tests) */
    PeakReport peak_{};
};

} // namespace cw
