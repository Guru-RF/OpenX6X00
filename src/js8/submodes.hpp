/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 speeds
 *
 *  Everything that changes with the speed lives in this one table, so frame
 *  building, tone generation, slot timing and receive matching can't drift
 *  apart. The figures are desktop JS8Call's (JS8Submode.cpp): 79 symbols per
 *  frame, sampled at 12 kHz.
 */

#pragma once

#include "js8_submode.h"

#include <cstdint>

namespace x6100 {
namespace js8 {

struct Speed { // one row of the speed table
    js8_speed_t id;              ///< JS8_SPEED_*
    const char *name;            ///< "Normal", ...
    char        letter;          ///< 'N', ...
    int         varicode;        ///< submode number desktop puts in frames: 0, 1, 2 or 4
    int         symbol_samples;  ///< samples per symbol at 12000 Hz
    int         period_s;        ///< TX/RX slot length
    int         start_delay_ms;  ///< how far into its slot a frame begins
    int         rx_threshold_hz; ///< decodes this close in offset come from one station
    bool        original_costas; ///< sync pattern: the original one for Normal, the modified one otherwise
    bool        heartbeats;      ///< false for Turbo, where desktop never sends them
    int         rx_mask;         ///< matching JS8_SUBMODE_* bit

    static constexpr double SAMPLE_RATE = 12000.0;
    static constexpr int    TOP_HZ      = 3000; ///< where the app's RX filter ends

    double symbol_seconds() const { return symbol_samples / SAMPLE_RATE; }
    double tone_spacing_hz() const { return SAMPLE_RATE / symbol_samples; }
    /// Eight tones, one tone spacing each (rounded down to whole Hz).
    int bandwidth_hz() const { return 96000 / symbol_samples; }
    int period_ms() const { return 1000 * period_s; }
    /// The top tone has to stay under TOP_HZ.
    int max_offset_hz() const { return TOP_HZ - bandwidth_hz(); }
    /// Air time of one 79-symbol frame.
    double frame_seconds() const { return 79.0 * symbol_seconds(); }
};

/// The table row for a speed; an out-of-range id gives Normal.
const Speed &speed(js8_speed_t which);
/// The row whose desktop submode number is `number`; Normal if none is.
const Speed &speed_from_varicode(int number);

} // namespace js8
} // namespace x6100
