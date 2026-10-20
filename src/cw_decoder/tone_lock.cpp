/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: tone lock.
 */

#include "tone_lock.h"

#include <algorithm>
#include <cstdio>

namespace cw {

namespace {

constexpr float LPF_HZ       = 100.0f;  /* baseband low-pass */
constexpr int   LPF_TAPS     = 81;
constexpr float START_HZ     = 670.0f;
constexpr float PLL_BW       = 0.001f;

constexpr float LEVEL_GAIN   = 0.125f;  /* smoothing of the run power: ~256 ms */
constexpr float ALIVE_SNR    = 4.0f;    /* smoothed run power over noise: the station is there */
/* The PLL is pulled in proportion to the run SNR: not at all at 2, fully from 5 */
constexpr float PLL_FROM_SNR = 2.0f;
constexpr float PLL_FULL_SNR = 5.0f;

/* Moving the lock needs the peak 2 bins away from it */
constexpr float MOVE_BINS    = 2.0f;

/* A strong station 50 Hz off leaks enough into our bin to look "alive" here.
 * Strong frames where our bin stays 12 dB under the peak count +1, others -2
 * (a keying edge splashes into our bin for a frame or two); at 32 (half a
 * second of key-down) the station here is taken as not there. */
constexpr float HERE_REL     = 0.063f;
constexpr int   ELSEWHERE    = 32;

} // namespace

ToneLock::ToneLock() {
    float fc = LPF_HZ / RX_RATE;

    nco_ = nco_crcf_create(LIQUID_NCO);
    lpf_ = firfilt_crcf_create_kaiser(LPF_TAPS, fc, 60.0f, 0.0f);
    firfilt_crcf_set_scale(lpf_, 2.0f * fc);
    if (!nco_ || !lpf_) {
        fprintf(stderr, "cw: no NCO/filter\n");
        return;
    }
    nco_crcf_pll_set_bandwidth(nco_, PLL_BW);
    tune(START_HZ);
}

ToneLock::~ToneLock() {
    if (nco_)
        nco_crcf_destroy(nco_);
    if (lpf_)
        firfilt_crcf_destroy(lpf_);
}

float ToneLock::hz() const {
    if (!nco_)
        return 0.0f;
    /* The NCO turns backwards (it mixes the tone down to 0 Hz) */
    return -nco_crcf_get_frequency(nco_) * RX_RATE / (2.0f * static_cast<float>(M_PI));
}

void ToneLock::tune(float hz) {
    if (!nco_)
        return;

    /* Same phase, new frequency */
    float phase = nco_crcf_get_phase(nco_);
    nco_crcf_reset(nco_);
    nco_crcf_set_frequency(nco_, 2.0f * static_cast<float>(M_PI) * (-hz / RX_RATE));
    nco_crcf_set_phase(nco_, phase);

    /* Nothing measured on the old tone may leak into the new one */
    firfilt_crcf_reset(lpf_);
    bb_.fill({});
    bb_pos_     = 0;
    since_      = 0;
    sum4_       = {};
    steps_      = 0;
    last_dir_   = {};
    level_set_  = false;
    alive_      = false;
    alive_runs_ = 0;
    elsewhere_  = 0;
}

void ToneLock::shift(float df) {
    bool alive = alive_;
    int  runs  = alive_runs_;
    int  quiet = quiet_runs_;

    tune(hz() + df);
    alive_      = alive;
    alive_runs_ = runs;
    quiet_runs_ = quiet;
}

bool ToneLock::follow(const PeakReport &peak) {
    if (!nco_)
        return false;

    if (peak.steady > 0) {
        elsewhere_ += peak.at_lock < HERE_REL ? 1 : -2;
        elsewhere_  = std::clamp(elsewhere_, 0, ELSEWHERE);
    }
    if (peak.steady < PEAK_STEADY_MIN)
        return false;

    /* Our station is (still) here: keep it, unless the strong frames show it is
     * only the skirt of another one */
    bool here = quiet_runs_ < GONE_RUNS && alive_runs_ >= HOLD_RUNS;
    if (here && elsewhere_ < ELSEWHERE)
        return false;

    if (std::fabs(peak.hz - hz()) < MOVE_BINS * BIN_HZ)
        return false;

    tune(peak.hz);
    return true;
}

float ToneLock::push(float x) {
    if (!nco_ || !lpf_)
        return -1.0f;

    float s, c;
    nco_crcf_sincos(nco_, &s, &c);
    std::complex<float> y = {x * c, -x * s};
    nco_crcf_step(nco_);

    firfilt_crcf_push(lpf_, y);
    firfilt_crcf_execute(lpf_, &y);

    bb_[bb_pos_]             = y;
    bb_[bb_pos_ + INTEG_LEN] = y;
    bb_pos_ = (bb_pos_ + 1) % INTEG_LEN;

    if (++since_ < INTEG_STEP)
        return -1.0f;
    since_ = 0;

    const std::complex<float> *w = &bb_[bb_pos_];   /* oldest first */
    std::complex<float> mean{};
    for (size_t i = 0; i < INTEG_LEN; i++)
        mean += w[i];
    mean /= static_cast<float>(INTEG_LEN);

    sum4_ += mean;
    if (++steps_ >= 4) {
        std::complex<float> m4 = sum4_ / 4.0f;
        steps_ = 0;
        sum4_  = {};
        every_4_steps(m4);
    }
    return std::norm(mean);
}

void ToneLock::every_4_steps(std::complex<float> mean) {
    float p = std::norm(mean);

    if (noise4_ <= 0.0f)
        return;

    if (level_set_) {
        level_ += LEVEL_GAIN * (p - level_);
    } else {
        level_     = p;
        level_set_ = true;
    }

    float snr   = p / noise4_;
    bool  alive = level_ / noise4_ > ALIVE_SNR;

    if (alive) {
        quiet_runs_ = 0;
        alive_runs_ = std::min(alive_runs_ + 1, HOLD_RUNS);
    } else if (quiet_runs_ < GONE_RUNS) {
        if (++quiet_runs_ == GONE_RUNS)
            alive_runs_ = 0;
    }

    if (p > 1e-8f) {
        std::complex<float> dir = mean / std::sqrt(p);

        /* Pull the NCO only between two runs with the station there: the first
         * run after a gap only sets the phase reference, so noise cannot kick it */
        if (alive_ && alive) {
            float pull = std::clamp((snr - PLL_FROM_SNR) / (PLL_FULL_SNR - PLL_FROM_SNR), 0.0f, 1.0f);
            if (pull > 0.0f) {
                float err = dir.imag() * last_dir_.real() - dir.real() * last_dir_.imag();
                nco_crcf_pll_step(nco_, pull * err);
            }
        }
        last_dir_ = dir;
    }
    alive_ = alive;
}

} // namespace cw
