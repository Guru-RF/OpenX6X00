/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: times the key edges into dits, dahs and gaps and learns the
 *  sender's speed.
 */

#pragma once

#include <array>

#include "rx_defs.h"

namespace cw {

/* Histogram of mark and gap lengths, 4 ms per slot (up to 1.2 s) */
constexpr size_t SLOTS   = 300;
constexpr float  SLOT_MS = STEP_MS / 2.0f;

/* Speed range: a dot of 24-120 ms (50-10 WPM), 20 WPM to start */
constexpr float  DOT_START_MS = PARIS_MS / 20.0f;
constexpr float  DOT_MIN_MS   = PARIS_MS / 50.0f;
constexpr float  DOT_MAX_MS   = PARIS_MS / 10.0f;

/* Every histogram entry ages by this when a new one comes in */
constexpr float  AGE = 0.9f;

/* Each step the timer gets the detector's edge (if any). The time of a step
 * that holds an edge is split at the crossing, so the closing mark or gap
 * gets its part and the new one the rest: marks and gaps of one unit then
 * fall on the same spot of the histogram. The dot length is the centre of
 * the lowest cluster there. */
class ElementTimer {
  public:
    /* dir +1 key down, -1 key up, 0 nothing; back_ms: the crossing lies that
     * long before this step; strong: (key up) the mark was well above the floor */
    Sym   step(int dir, float back_ms, bool strong = true);

    float wpm() const { return static_cast<float>(PARIS_MS) / dot_ms_; }
    bool  key_down() const { return down_; }

    /* The lock moved to another tone: the running mark or gap is not learned
     * (it may have been measured on the old tone), the speed is kept */
    void  new_tone() { unsure_ = true; }

  private:
    friend struct ElementTimerPeek;   /* tests only */

    Sym   close(bool mark, float ms, bool learn);
    void  learn(float ms);
    float dot_estimate(float min_peak) const;
    void  set_dot(float ms);
    bool  backed(float ms) const;
    void  retime();

    std::array<float, SLOTS> hist_{};
    int   hist_n_  = 0;     /* entries since the histogram was last emptied */
    float age_     = AGE;

    bool  down_    = false;
    float run_ms_  = 0.0f;  /* length of the running mark or gap */
    bool  ended_   = true;  /* the end of the text was already sent for this gap */
    bool  unsure_  = false; /* the running mark or gap began on another tone */

    /* A mark with a word gap before it is held until we know whether more
     * follows: alone it is an E or a T, or a noise blip */
    bool  first_       = true;
    bool  held_        = false;
    float held_ms_     = 0.0f;
    bool  held_learn_  = false;
    bool  held_strong_ = false;
    Sym   next_        = SYM_NONE;  /* a second symbol due in the same step: sent with the next one */

    /* The last gaps of a character gap or longer: their median is the
     * character gap, which slow (Farnsworth) sending stretches */
    std::array<float, 16> gaps_{};
    int   gaps_n_   = 0;
    int   gaps_pos_ = 0;

    float dot_ms_   = DOT_START_MS;
    float new_dot_  = -1.0f;   /* a different speed, waiting to be confirmed */
    int   new_seen_ = 0;

    float dah_from_  = 2.0f * DOT_START_MS;  /* mark: dit under, dah over */
    float char_from_ = 1.7f * DOT_START_MS;  /* gap: character gap from here */
    float word_from_ = 5.0f * DOT_START_MS;  /* gap: word gap from here */
};

} // namespace cw
