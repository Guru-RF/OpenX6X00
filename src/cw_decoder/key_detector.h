/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: key up / key down from the tone lock's power.
 */

#pragma once

#include <array>

#include "rx_defs.h"

namespace cw {

/* A key edge found in one step */
struct KeyEdge {
    int   dir;      /* +1 key down, -1 key up, 0 none */
    float back_ms;  /* the crossing lies this long before the start of the step (>= 0) */
    bool  strong;   /* (key up) the mark peaked 3 dB over the decode floor */
};

/* Two thresholds that follow the mark's peak (-3 dB to go down, -6 dB to
 * come up again) but never sit under the user's decode floor; two steps over
 * a threshold make an edge, and the exact crossing is interpolated back
 * between the last step on the old side and the first on the new one.
 * A steady carrier on the tone (a birdie) is kept from keying by a second
 * floor: the low 5% of power/noise, which CW keying leaves at the noise. */
class KeyDetector {
  public:
    /* power and noise of one integrator step */
    KeyEdge step(float power, float noise);

    void  set_floor_db(float db) { floor_db_ = db; }
    float floor_db() const { return floor_db_; }
    bool  down() const { return down_; }

    /* Another tone: measure its carrier floor again */
    void  new_tone() { batch_n_ = 0; }
    /* Noise in other units (APF switched): wait and measure again */
    void  restart() {
        batch_n_ = 0;
        muted_   = true;
        carrier_ = 0.0f;
    }

    /* Last step, for the tests and the SNR shown */
    float on_level() const { return on_lvl_; }
    float off_level() const { return off_lvl_; }
    float power() const { return last_power_; }

  private:
    friend struct KeyDetectorPeek;   /* tests only */

    KeyEdge schmitt(float r);
    float   crossing() const;
    void    follow_peak(float r);

    /* Schmitt trigger */
    float  floor_db_  = 5.0f;
    float  on_lvl_    = 4.0f;
    float  off_lvl_   = 2.0f;
    float  mid_lvl_   = 2.0f;   /* where the crossing is placed */
    float  peak_      = 10.0f;
    bool   down_      = false;
    size_t run_       = 0;      /* steps over the threshold toward the other side */
    size_t up_steps_  = 0;      /* steps since key up */
    float  r_old_     = 0.0f;   /* last step on the old side */
    float  r_new_     = 0.0f;   /* first step on the new side */

    /* Carrier floor */
    static constexpr int BATCH = 125;   /* 1 s of steps */
    std::array<float, BATCH> batch_{};
    int    batch_n_   = 0;
    bool   muted_     = true;
    float  carrier_   = 0.0f;   /* 5th percentile of power/noise */
    int    down_steps_ = 0;

    float  mark_peak_  = 0.0f;
    float  last_power_ = 0.0f;
};

} // namespace cw
