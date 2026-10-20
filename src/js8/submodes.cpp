/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 speeds
 */

#include "submodes.hpp"

#include "js8_receive.h"

namespace x6100 {
namespace js8 {

// Indexed by js8_speed_t. Symbol length, start delay, slot and threshold as in
// desktop's JS8Submode.cpp (its rxThreshold is 10 Hz where it sets none).
static constexpr Speed TABLE[] = {
    {.id = JS8_SPEED_NORMAL, .name = "Normal", .letter = 'N', .varicode = 0, .symbol_samples = 1920, .period_s = 15,
     .start_delay_ms = 500, .rx_threshold_hz = 10, .original_costas = true, .heartbeats = true,
     .rx_mask = JS8_SUBMODE_NORMAL},
    {.id = JS8_SPEED_FAST, .name = "Fast", .letter = 'F', .varicode = 1, .symbol_samples = 1200, .period_s = 10,
     .start_delay_ms = 200, .rx_threshold_hz = 16, .original_costas = false, .heartbeats = true,
     .rx_mask = JS8_SUBMODE_FAST},
    {.id = JS8_SPEED_TURBO, .name = "Turbo", .letter = 'T', .varicode = 2, .symbol_samples = 600, .period_s = 6,
     .start_delay_ms = 100, .rx_threshold_hz = 32, .original_costas = false, .heartbeats = false,
     .rx_mask = JS8_SUBMODE_TURBO},
    {.id = JS8_SPEED_SLOW, .name = "Slow", .letter = 'S', .varicode = 4, .symbol_samples = 3840, .period_s = 30,
     .start_delay_ms = 500, .rx_threshold_hz = 10, .original_costas = false, .heartbeats = true,
     .rx_mask = JS8_SUBMODE_SLOW},
};
static_assert(sizeof TABLE / sizeof TABLE[0] == JS8_SPEED_COUNT);

const Speed &speed(js8_speed_t which) {
    // The unsigned cast also sends negative values to the fallback.
    const unsigned i = static_cast<unsigned>(which);
    return i < JS8_SPEED_COUNT ? TABLE[i] : TABLE[JS8_SPEED_NORMAL];
}

const Speed &speed_from_varicode(int number) {
    for (const Speed &row : TABLE)
        if (row.varicode == number) return row;
    return TABLE[JS8_SPEED_NORMAL];
}

} // namespace js8
} // namespace x6100

namespace sp = x6100::js8;

const char *js8_speed_name(js8_speed_t speed) { return sp::speed(speed).name; }
char js8_speed_letter(js8_speed_t speed) { return sp::speed(speed).letter; }
int js8_speed_bandwidth_hz(js8_speed_t speed) { return sp::speed(speed).bandwidth_hz(); }
int js8_speed_period_s(js8_speed_t speed) { return sp::speed(speed).period_s; }
int js8_speed_rx_threshold_hz(js8_speed_t speed) { return sp::speed(speed).rx_threshold_hz; }
int js8_speed_max_offset_hz(js8_speed_t speed) { return sp::speed(speed).max_offset_hz(); }
bool js8_speed_heartbeats(js8_speed_t speed) { return sp::speed(speed).heartbeats; }
int js8_speed_rx_mask(js8_speed_t speed) { return sp::speed(speed).rx_mask; }
int js8_speed_submode(js8_speed_t speed) { return sp::speed(speed).varicode; }
js8_speed_t js8_speed_from_submode(int submode_number) { return sp::speed_from_varicode(submode_number).id; }
