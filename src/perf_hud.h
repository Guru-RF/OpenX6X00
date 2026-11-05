/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Performance overlay: frames per second, draw time, GUI loop time and the time from a turn of
 *  the main knob to the frame that shows it. On when /mnt/perf exists or X6100_PERF=1; the
 *  numbers also go to the log every 10 s.
 */
#pragma once

#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdint.h>

void perf_hud_init(lv_disp_drv_t *drv);
bool perf_hud_on(void);

/* The GUI loop: how long one pass took (ms) */
void perf_loop_time(uint32_t ms);

/* The main knob: a detent read by its thread at t_us (CLOCK_MONOTONIC, any thread) ... */
void perf_knob_seen(uint64_t t_us);
/* ... and handed to the GUI: the next frame drawn ends the measurement */
void perf_knob_used(void);
