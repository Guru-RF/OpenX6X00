/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  LVGL's clock read from the kernel's monotonic clock (lv_conf.h, LV_TICK_CUSTOM). It was a
 *  thread adding 5 ms after each usleep(5000): every sleep runs over and the overrun was lost, so
 *  LVGL's time ran slow (1.5% on an X6200, 17-28% on a PC in a VM), and with it the screen
 *  refresh, the key and knob reads, the timers and the animations.
 */
#pragma once

#include <stdint.h>
#include <time.h>

static inline uint32_t lv_tick_mono_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}
