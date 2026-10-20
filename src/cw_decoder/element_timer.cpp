/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: element timer.
 */

#include "element_timer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace cw {

namespace {

/* Boundaries in dots: dit/dah at 2, element/character gap at 1.7 (a bit short,
 * so two letters are not run together), character/word gap at 5 */
constexpr float DAH_K  = 2.0f;
constexpr float CHAR_K = 1.7f;
constexpr float WORD_K = 5.0f;

/* Each entry is spread over +-4 slots (a triangle), so a mark and a gap of the
 * same length still meet when the step grid puts them a slot apart */
constexpr int   SPREAD = 4;

/* Peaks lower than 8% of the highest are tails of the spread, not clusters */
constexpr float MIN_PEAK = 0.08f;
constexpr float RELOCK_PEAK = 0.25f;
/* The next cluster (a dah or a character gap, ~3 dots) starts at 1.7x the first */
constexpr float NEXT_CLUSTER = 1.7f;
/* With no next cluster, the dot cluster ends at 2.2x its peak */
constexpr float CLUSTER_END = 2.2f;

/* A dot estimate more than 25% off is a new speed: taken after 3 in a row, and
 * then the histogram starts over, so two speeds are not averaged */
constexpr float NEW_SPEED = 0.25f;
constexpr int   CONFIRM   = 3;
/* ... but only once the histogram holds 8 entries: right after starting over
 * the first dah alone looks like a new speed, and the estimate would swing
 * between the two forever */
constexpr int   MIN_ENTRIES = 8;
constexpr float DOT_GAIN = 0.3f;   /* smoothing of the dot length */

/* A lone mark is a letter only if it lasts 0.7 dots; longer than 8 dots it is
 * a carrier (a birdie, someone tuning), never a letter */
constexpr float LONE_MIN  = 0.7f;
constexpr float CARRIER_K = 8.0f;

/* Character gaps needed before they may move the word boundary */
constexpr int   GAPS_MIN = 8;
constexpr float FARNS_K  = 5.0f / 3.0f;
constexpr float FARNS_MAX_MS = 3000.0f;   /* 5 WPM Farnsworth needs 2.5 s */

/* End of text: no mark after 5 character gaps (at least 200 ms, and never
 * before the word boundary) */
constexpr float END_K      = 5.0f;
constexpr float END_MIN_MS = 200.0f;

} // namespace

void ElementTimer::retime() {
    dah_from_  = DAH_K * dot_ms_;
    char_from_ = CHAR_K * dot_ms_;
    word_from_ = WORD_K * dot_ms_;

    /* Farnsworth: characters at speed, the gaps stretched (the ARRL sends 10 WPM
     * as 18 WPM characters with 9-dot character gaps and 22-dot word gaps). With
     * character gaps over 4 dots the word boundary moves up with them: 5/3 of
     * (gap - 1 dot) sits between the two, and is 5 dots at 4 dots. */
    if (gaps_n_ >= GAPS_MIN) {
        std::array<float, 16> g = gaps_;
        auto mid = g.begin() + gaps_n_ / 2;
        std::nth_element(g.begin(), mid, g.begin() + gaps_n_);
        word_from_ = std::max(word_from_, std::min(FARNS_K * (*mid - dot_ms_), FARNS_MAX_MS));
    }
}

/* A new speed must also be what the histogram shows without its small peaks:
 * a few fragments of marks broken up by a fade make a small peak low down,
 * and the speed would jump to it */
bool ElementTimer::backed(float ms) const {
    float strict = dot_estimate(RELOCK_PEAK);

    return strict >= 0.0f && std::fabs(std::clamp(strict, DOT_MIN_MS, DOT_MAX_MS) - ms) <= NEW_SPEED * ms;
}

void ElementTimer::set_dot(float ms) {
    if (std::fabs(ms - dot_ms_) <= NEW_SPEED * dot_ms_) {
        dot_ms_   = std::clamp(dot_ms_ + DOT_GAIN * (ms - dot_ms_), DOT_MIN_MS, DOT_MAX_MS);
        new_dot_  = -1.0f;
        new_seen_ = 0;
    } else if (hist_n_ < MIN_ENTRIES) {
        new_dot_  = -1.0f;
        new_seen_ = 0;
        return;
    } else if (!backed(ms)) {
        new_dot_  = -1.0f;
        new_seen_ = 0;
        return;
    } else {
        bool again = new_seen_ > 0 && std::fabs(ms - new_dot_) <= NEW_SPEED * new_dot_;
        new_dot_   = ms;
        new_seen_  = again ? new_seen_ + 1 : 1;
        if (new_seen_ >= CONFIRM) {
            hist_.fill(0.0f);
            hist_n_   = 0;
            gaps_n_   = 0;
            dot_ms_   = ms;
            new_dot_  = -1.0f;
            new_seen_ = 0;
        }
    }
    retime();
}

void ElementTimer::learn(float ms) {
    /* Everything ages, even when the new entry falls outside */
    for (float &h : hist_)
        h *= age_;
    if (ms <= 0.0f)
        return;

    int mid = static_cast<int>(std::lround(ms / SLOT_MS));
    if (mid < 0 || mid >= static_cast<int>(SLOTS))
        return;
    hist_n_++;

    /* Weights of the slots that exist, so each entry adds exactly 1 */
    float total = 0.0f;
    for (int k = -SPREAD; k <= SPREAD; k++) {
        int s = mid + k;
        if (s >= 0 && s < static_cast<int>(SLOTS))
            total += static_cast<float>(SPREAD + 1 - std::abs(k));
    }
    if (total <= 0.0f)
        return;
    for (int k = -SPREAD; k <= SPREAD; k++) {
        int s = mid + k;
        if (s >= 0 && s < static_cast<int>(SLOTS))
            hist_[s] += static_cast<float>(SPREAD + 1 - std::abs(k)) / total;
    }
}

/* Centre of mass of the lowest cluster: dots and element gaps (marks come out
 * a little long, gaps as much short) make one cluster whose top alone could
 * sit on either side, the centre does not. The cluster runs from 0 to the
 * lowest point before the next cluster. */
float ElementTimer::dot_estimate(float min_frac) const {
    const int n = static_cast<int>(SLOTS);

    float top = *std::max_element(hist_.begin(), hist_.end());
    if (top <= 0.0f)
        return -1.0f;
    float min_peak = min_frac * top;

    auto is_peak = [&](int i) {
        float l = i > 0 ? hist_[i - 1] : 0.0f;
        float r = i + 1 < n ? hist_[i + 1] : 0.0f;
        return hist_[i] >= min_peak && hist_[i] > l && hist_[i] >= r;
    };

    int first = 0;
    while (first < n && !is_peak(first))
        first++;
    if (first == n)
        return -1.0f;
    float first_ms = static_cast<float>(first) * SLOT_MS;

    int next = first + 1;
    while (next < n && !(is_peak(next) && static_cast<float>(next) * SLOT_MS >= NEXT_CLUSTER * first_ms))
        next++;
    if (next == n)
        next = std::min(static_cast<int>(std::lround(CLUSTER_END * first_ms / SLOT_MS)), n - 1);
    if (next <= first)
        next = std::min(first + 1, n - 1);

    int low = first;
    for (int i = first; i <= next; i++)
        if (hist_[i] < hist_[low])
            low = i;

    float mass = 0.0f, moment = 0.0f;
    for (int i = 0; i <= low; i++) {
        mass += hist_[i];
        moment += static_cast<float>(i) * hist_[i];
    }
    if (mass <= 0.0f)
        return -1.0f;
    return (moment / mass) * SLOT_MS;
}

Sym ElementTimer::close(bool mark, float ms, bool learn_it) {
    Sym s;

    if (mark)
        s = ms < dah_from_ ? SYM_DIT : SYM_DAH;
    else if (ms < char_from_)
        s = SYM_ELEM_GAP;
    else
        s = ms < word_from_ ? SYM_CHAR_GAP : SYM_WORD_GAP;

    if (learn_it) {
        if (!mark && ms >= char_from_) {
            gaps_[gaps_pos_] = ms;
            gaps_pos_        = (gaps_pos_ + 1) % static_cast<int>(gaps_.size());
            gaps_n_          = std::min(gaps_n_ + 1, static_cast<int>(gaps_.size()));
        }
        learn(ms);
        float dot = dot_estimate(MIN_PEAK);
        if (dot >= 0.0f)
            set_dot(std::clamp(dot, DOT_MIN_MS, DOT_MAX_MS));
    }
    return s;
}

Sym ElementTimer::step(int dir, float back_ms, bool strong) {
    Sym out = next_;
    next_   = SYM_NONE;

    /* Two symbols in one step: the second waits for the next step */
    auto send = [&](Sym s) {
        if (out == SYM_NONE)
            out = s;
        else
            next_ = s;
    };

    if (dir == 0) {
        run_ms_ += static_cast<float>(STEP_MS);
    } else {
        /* The crossing was back_ms before this step: that much belongs to the
         * new mark or gap, not to the one closing */
        run_ms_ = std::max(run_ms_ - back_ms, 0.0f);

        if (dir > 0 && held_) {
            held_ = false;
            if (held_strong_) {
                /* More keying after the held mark: it was a real one */
                out    = close(true, held_ms_, held_learn_);
                next_  = close(false, run_ms_, !unsure_);
                first_ = false;
            }
            /* else too weak to open a text: dropped, the new mark is the first */
        } else if (dir < 0 && run_ms_ > CARRIER_K * dot_ms_) {
            first_ = true;     /* a carrier, not keying */
        } else if (dir < 0 && first_) {
            held_        = true;
            held_ms_     = run_ms_;
            held_learn_  = !unsure_;
            held_strong_ = strong;
        } else if (down_ || !ended_) {
            /* (a gap whose end of text was already sent is not closed again) */
            Sym s = close(down_, run_ms_, !unsure_);
            send(s);
            if (dir > 0)
                first_ = s == SYM_WORD_GAP;
        } else {
            first_ = true;     /* after the end of a text */
        }

        unsure_ = false;
        down_   = dir > 0;
        run_ms_ = static_cast<float>(STEP_MS) + back_ms;
        if (down_)
            ended_ = false;
    }

    /* The held mark stayed alone for a word gap: a one-element word if it is
     * long enough at this speed, else a blip. Never learned: blips would pull
     * the speed up, and then more blips would pass. */
    if (held_ && run_ms_ >= word_from_) {
        held_ = false;
        if (held_strong_ && held_ms_ >= LONE_MIN * dot_ms_)
            out = close(true, held_ms_, false);
    }

    /* No mark after a long gap: end of the text, sent once so the last
     * character comes out */
    if (!down_ && !ended_ && run_ms_ >= std::max({END_K * char_from_, END_MIN_MS, word_from_})) {
        ended_  = true;
        run_ms_ = 0.0f;
        send(SYM_WORD_GAP);
    }
    return out;
}

} // namespace cw
