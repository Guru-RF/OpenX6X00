/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The processor's speed: Settings > General > CONTROLS > Processor (cpu_power.c)
 *
 *  Full speed: the kernel's "performance" governor, the four cores at 1008 MHz all the time, as the radios' kernels
 *  start and as every version before had it. Saves power: "ondemand", which looks at the load every 20 ms (two ticks
 *  of the radios' 100 Hz kernel), goes straight to the top as soon as a core was busy more than 80 % of that time,
 *  and below that clocks down in proportion to the load; never under CPU_SAVE_MIN_KHZ.
 *
 *  The floor: the same cores take the base's data from its serial port (1.152 Mbaud), the audio and the GUI's frames.
 *  At the bottom step, 120 MHz, all of that would run up to eight times slower until the next look at the load; at
 *  half the top speed at most twice, a knob's first frame after a quiet moment too.
 *
 *  What it saves on these radios: the four cores share one clock, and their device trees give the kernel no
 *  regulator for them ("cpu-supply"), so the voltage stays at U-Boot's 1.2 V at every step. A slower clock saves
 *  what the clock costs while the cores wait, not the voltage's share.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_FULL_GOV        "performance"
#define CPU_SAVE_GOV        "ondemand"
#define CPU_SAVE_MIN_KHZ    480000UL

/* The lowest of the frequencies a policy offers (scaling_available_frequencies, kHz, space separated) at
 * CPU_SAVE_MIN_KHZ or above; none that high, its highest; no list, CPU_SAVE_MIN_KHZ within min_khz..max_khz */
unsigned long cpu_power_floor_khz(const char *avail, unsigned long min_khz, unsigned long max_khz);

/* Every cpufreq policy under cpufreq_dir (policy0 on the radios: one for the four cores) to Saves power (save) or Full
 * speed. False: none found, or one not set (a governor the kernel has not got: that policy stays as it was) */
bool cpu_power_apply(const char *cpufreq_dir, bool save);

/* LVGL thread, once the settings are read: Saves power applied now if chosen, and either choice from then on as it is
 * made (in Settings or on the web configurator's Display page). Full speed at the start leaves the kernel's own
 * setting as it is */
void cpu_power_start(void);

#ifdef __cplusplus
}
#endif
