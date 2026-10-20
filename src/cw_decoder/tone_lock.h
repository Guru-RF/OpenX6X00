/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: coherent detector locked on one station's tone.
 */

#pragma once

#include <array>
#include <complex>

#include "rx_defs.h"

#include <liquid/liquid.h>

namespace cw {

/* Mixes the audio down with an NCO on the station's tone, low-passes it
 * (100 Hz) and averages INTEG_LEN samples: the power of that mean is what
 * the keying detector sees, every INTEG_STEP samples. Every 4 steps the
 * mean feeds a PLL that keeps the NCO on the tone while the station is
 * there, and a liveness test that keeps a station from being dropped for a
 * stronger peak somewhere else in the band. */
class ToneLock {
  public:
    ToneLock();
    ~ToneLock();
    ToneLock(const ToneLock &)            = delete;
    ToneLock &operator=(const ToneLock &) = delete;

    /* One sample in; every INTEG_STEP samples the baseband power, else -1 */
    float push(float x);

    /* Noise power of one integrator step (from the tone finder) */
    void  set_noise(float step_noise) { noise4_ = step_noise * NOISE_4STEPS; }

    /* Moves to the FFT peak when it is steady, away from the NCO and the
     * station here is gone (or was never really here). True if it moved. */
    bool  follow(const PeakReport &peak);

    /* A new station at hz: the lock starts over */
    void  tune(float hz);

    /* The VFO moved by df Hz of audio: same station, so it stays locked */
    void  shift(float df);

    float hz() const;

    /* A tone has been held here for a while (256 ms): noise keying the
     * detector seldom stays coherent that long */
    bool  holding() const { return alive_ && alive_runs_ >= HOLD_RUNS; }

  private:
    static constexpr int HOLD_RUNS = 8;     /* 4-step runs: 256 ms */
    static constexpr int GONE_RUNS = 32;    /* runs without the tone before it may be dropped: 1 s */

    void every_4_steps(std::complex<float> mean);

    nco_crcf     nco_ = nullptr;
    firfilt_crcf lpf_ = nullptr;

    /* Baseband samples twice over, so the last INTEG_LEN are contiguous */
    std::array<std::complex<float>, 2 * INTEG_LEN> bb_{};
    size_t bb_pos_ = 0;
    size_t since_  = 0;               /* samples since the last step */

    std::complex<float> sum4_{};      /* sum of the step means of this run */
    int                 steps_ = 0;   /* steps in this run */
    std::complex<float> last_dir_{};  /* unit phasor of the previous run */

    float noise4_     = 0.0f;         /* noise of a 4-step mean, <= 0 = not known yet */
    float level_      = 0.0f;         /* smoothed power of the run means */
    bool  level_set_  = false;
    bool  alive_      = false;        /* the station was there in the last run */
    int   quiet_runs_ = GONE_RUNS;    /* runs since it was last there (starts "gone") */
    int   alive_runs_ = 0;            /* runs it has been there, up to HOLD_RUNS */
    int   elsewhere_  = 0;            /* evidence that the strong frames are not here */
};

} // namespace cw
