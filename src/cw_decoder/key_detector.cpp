/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: key detector.
 */

#include "key_detector.h"

#include <algorithm>

namespace cw {

namespace {

constexpr size_t EDGE_STEPS  = 2;    /* steps over a threshold for an edge */
/* After 400 ms of key up (about a word gap) the first mark needs one step more:
 * clicks and noise peaks in a quiet band last two steps */
constexpr size_t QUIET_STEPS = 50;

/* Carrier floor: noise alone has its 5th percentile of power/noise at
 * -ln(0.95) = 0.0513. Twice that and the noise reference is raised with it. */
constexpr float  PCT         = 0.05f;
constexpr float  PCT_NOISE   = 0.05129f;
constexpr float  CARRIER_K   = 2.0f;
constexpr float  PCT_STEP    = 0.02f;
constexpr int    CARRIER_STEPS = 60;    /* 480 ms down, longer than a 10 WPM dash */
constexpr float  CARRIER_UP  = 1.05f;   /* per step, once it is a carrier */

/* A mark alone must peak this much over the decode floor (see ElementTimer) */
constexpr float  LONE_MARK_DB = 3.0f;

} // namespace

KeyEdge KeyDetector::step(float power, float noise) {
    float r = power / noise;

    if (batch_n_ < BATCH) {
        /* First second on a tone: the floor from a batch, tracked from there */
        batch_[batch_n_++] = r;
        if (batch_n_ == BATCH) {
            auto q = batch_.begin() + static_cast<long>(PCT * BATCH);
            std::nth_element(batch_.begin(), q, batch_.end());
            carrier_ = std::max({carrier_, *q, 1e-3f * PCT_NOISE});
            muted_   = false;
        }
    }
    if (muted_) {
        power = 0.0f;   /* the noise floors are still settling */
    } else if (down_steps_ > CARRIER_STEPS) {
        carrier_ *= CARRIER_UP;
    } else {
        /* Quantile tracking: up a little when over, down when under */
        carrier_ *= r > carrier_ ? (1.0f + PCT_STEP * PCT) : (1.0f - PCT_STEP * (1.0f - PCT));
        carrier_ = std::max(carrier_, 1e-3f * PCT_NOISE);
    }
    last_power_ = power;

    float n  = noise * std::max(1.0f, carrier_ / (CARRIER_K * PCT_NOISE));
    float rn = power / n;

    KeyEdge e = schmitt(rn);

    if (e.dir > 0)
        mark_peak_ = rn;
    else if (down_ || e.dir < 0)
        mark_peak_ = std::max(mark_peak_, rn);
    e.strong = mark_peak_ >= from_db(floor_db_ + LONE_MARK_DB);

    down_steps_ = down_ ? down_steps_ + 1 : 0;
    return e;
}

KeyEdge KeyDetector::schmitt(float r) {
    float back    = 0.0f;   /* samples */
    int   dir     = 0;
    bool  leaving = false;

    if (!down_) {
        size_t need = up_steps_ >= QUIET_STEPS ? EDGE_STEPS + 1 : EDGE_STEPS;

        up_steps_++;
        if (r > on_lvl_) {
            leaving = true;
            if (run_ == 0)
                r_new_ = r;
            if (++run_ >= need) {
                /* the extra step of a first mark is given back */
                back      = -crossing() + static_cast<float>((need - EDGE_STEPS) * INTEG_STEP);
                up_steps_ = 0;
                down_     = true;
                run_      = 0;
                dir       = 1;
            }
        } else {
            run_ = 0;
        }
    } else {
        if (r < off_lvl_) {
            leaving = true;
            if (run_ == 0)
                r_new_ = r;
            if (++run_ >= EDGE_STEPS) {
                back  = -crossing();
                down_ = false;
                run_  = 0;
                dir   = -1;
            }
        } else {
            run_ = 0;
        }
    }
    if (!leaving)
        r_old_ = r;

    follow_peak(r);
    return {dir, 1000.0f * back / RX_RATE, false};
}

/* Thresholds from the mark's peak, which decays slowly (faster with key up) */
void KeyDetector::follow_peak(float r) {
    if (down_ && r > peak_)
        peak_ = r;
    peak_ *= down_ ? 0.97f : 0.94f;
    if (peak_ < 1.0f)
        peak_ = 1.0f;

    on_lvl_  = std::max(peak_ * from_db(-3.0f), from_db(floor_db_));
    off_lvl_ = std::max(peak_ * from_db(-6.0f), from_db(floor_db_ - 3.0f));
    mid_lvl_ = 1.0f + (peak_ - 1.0f) * from_db(-6.0f);
}

/* Where between the last old-side step and the first new-side step the
 * envelope crossed mid_lvl_, in samples back from the newer one (<= 0). The
 * square roots turn power ratios into amplitudes, which ramp about linearly
 * on a keyed edge. */
float KeyDetector::crossing() const {
    const float step = static_cast<float>(INTEG_STEP);
    const float span = static_cast<float>(INTEG_LEN);

    float a_new = std::sqrt(std::max(r_new_ - 1.0f, 0.0f));
    float a_old = std::sqrt(std::max(r_old_ - 1.0f, 0.0f));
    float a_mid = std::sqrt(std::max(mid_lvl_ - 1.0f, 0.0f));
    float slope = a_new - a_old;

    if (std::abs(slope) < 1e-3f)
        return -step;

    float t = -step * (a_new - a_mid) / slope;
    return std::clamp(t, -(span + step), 0.0f);
}

} // namespace cw
