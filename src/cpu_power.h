/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The processor at full speed (cpu_power.c)
 *
 *  The radios run their four cores at 1008 MHz all the time: the kernel's "performance" governor, as their kernels
 *  start. v0.3.11 had a Processor setting whose Saves power put the kernel's "ondemand" governor on from 480 MHz up.
 *  On the bench X6200 it ran no cooler, the base's data came late about once a second ("Flow reset" in the log) and
 *  once the audio server stopped, so v0.3.12 took it out. A radio that v0.3.11 left on it runs at full speed again from
 *  the next start of the GUI (an update restarts it), without a restart of the radio.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_FULL_GOV "performance"
#define CPU_SAVE_GOV "ondemand" /* v0.3.11's Saves power */

/* Every cpufreq policy under cpufreq_dir that v0.3.11's Saves power left behind (CPU_SAVE_GOV with its floor above the
 * kernel's own, cpuinfo_min_freq) back to full speed: CPU_FULL_GOV and the kernel's floor. Any other governor, one set
 * by hand over SSH too, is left as it is. The number of policies put back; -1: no policy found, or one that could not
 * be read or written */
int cpu_power_restore(const char *cpufreq_dir);

/* LVGL thread, at the start: cpu_power_restore on the radio's cpufreq files (the PC build leaves its own alone) */
void cpu_power_start(void);

#ifdef __cplusplus
}
#endif
