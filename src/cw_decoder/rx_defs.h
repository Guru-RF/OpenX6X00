/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: rates, block sizes and the small types the parts share.
 */

#pragma once

#include <cmath>
#include <cstddef>

namespace cw {

/* The receiver runs at 4 kHz: a CW tone sits in 300-1500 Hz and the
 * coherent detector needs no more. */
constexpr float  RX_RATE   = 4000.0f;

/* Tone search: 256-point FFT (15.6 Hz bins, 64 ms) every 64 samples (16 ms) */
constexpr size_t FFT_LEN   = 256;
constexpr size_t FFT_STEP  = 64;
constexpr size_t FFT_BINS  = FFT_LEN / 2 + 1;
constexpr float  BIN_HZ    = RX_RATE / static_cast<float>(FFT_LEN);

/* Keying detector: mean of the last 64 baseband samples (16 ms) every 32 (8 ms).
 * One such step is the time unit of the detector and of the element timer. */
constexpr size_t INTEG_LEN  = 64;
constexpr size_t INTEG_STEP = 32;
constexpr int    STEP_MS    = static_cast<int>(INTEG_STEP * 1000 / RX_RATE);

/* PARIS: a dot lasts 1200 / WPM ms */
constexpr int    PARIS_MS   = 1200;

/* Per-bin noise of the FFT -> noise of one integrator step. The FFT noise is
 * the 25th percentile of the band, which for exponential power is the mean
 * times ln(4/3) = 0.28768; the Hann window widens a bin to 1.5 bins; the
 * integrator averages INTEG_LEN samples where the FFT sums FFT_LEN. */
constexpr float  NOISE_TO_STEP = (1.0f / 0.28768207f) * static_cast<float>(FFT_LEN) /
                                 (1.5f * static_cast<float>(INTEG_LEN));
/* Mean of 4 half-overlapping steps: noise power drops to 7/16, not 1/4 */
constexpr float  NOISE_4STEPS  = 7.0f / 16.0f;

/* A peak this far above the noise (dB) may move the lock: tracking does not
 * depend on the user's decode floor */
constexpr float  PEAK_STRONG_DB  = 15.5f;
/* Frames in a row the peak must hold its bin before the lock follows it */
constexpr int    PEAK_STEADY_MIN = 2;

inline float from_db(float db) {
    return powf(10.0f, db * 0.1f);
}

/* What one key element / gap is, as the timer classifies it */
enum Sym {
    SYM_NONE,
    SYM_DIT,
    SYM_DAH,
    SYM_ELEM_GAP,   /* inside a character */
    SYM_CHAR_GAP,   /* between characters */
    SYM_WORD_GAP
};

/* One FFT frame as the tone finder saw it */
struct PeakReport {
    float hz;        /* strongest bin, interpolated */
    float snr;       /* its power over the per-bin noise */
    int   steady;    /* strong frames in a row on (about) the same bin, 0 = weak frame */
    float at_lock;   /* power in the locked tone's bin over the peak's, 0 when outside the band */
};

} // namespace cw
