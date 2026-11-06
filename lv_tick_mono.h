/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  LVGL's clock, the kernel's monotonic clock in ms (lv_conf.h, LV_TICK_CUSTOM). It was a thread adding 5 ms after
 *  each usleep(5000): every sleep runs over and the overrun was lost, so LVGL's time ran slow (1.5% on an X6200,
 *  17-28% on a PC in a VM), and with it the screen refresh, the key and knob reads, the timers and the animations.
 *
 *  lv_tick_get() reads the clock as last read, not the clock itself: the radios' kernels have no vDSO
 *  (CONFIG_VDSO is not set in both 6.1.82 configs), so each clock_gettime() is a system call, and LVGL reads its
 *  clock some 20 000 times a second, more while a knob turns. The GUI loop reads the clock before each
 *  lv_timer_handler() (lv_tick_mono_set, with the time it has read anyway), so the timers run on the clock's own
 *  time; a 5 ms thread (gui_main.c) keeps it going through a long frame and for the other threads that read it.
 *  Never back: the two writers may store out of order.
 */
#pragma once

#include <stdint.h>
#include <time.h>

extern uint32_t lv_tick_mono_now;      /* gui_main.c */

static inline uint32_t lv_tick_mono_ms(void) {
    return __atomic_load_n(&lv_tick_mono_now, __ATOMIC_RELAXED);
}

/* ms: CLOCK_MONOTONIC in ms (get_time()); later than the value stored, or nothing changes. 0 is none yet: the
 * first is taken whatever it is (an uptime over 24.8 days compares as earlier) */
static inline void lv_tick_mono_set(uint64_t ms) {
    uint32_t v = (uint32_t)ms, old = __atomic_load_n(&lv_tick_mono_now, __ATOMIC_RELAXED);

    while ((old == 0 || (int32_t)(v - old) > 0) &&
           !__atomic_compare_exchange_n(&lv_tick_mono_now, &old, v, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static inline void lv_tick_mono_read(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    lv_tick_mono_set((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}
