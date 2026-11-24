/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Load meter (load_meter.c, #233)
 *
 *  Settings > Info > SYSTEM > Load meter: thin bars at the left edge of the screen, over every screen (the main
 *  screen, the apps, Settings): C the processor (all its cores), G the GUI thread (of one core), M the memory in use,
 *  S the swap where the radio has any. C and G have a tick at their highest of the last 10 seconds. Under them a dot
 *  lights when the GUI loop was late in the last 10 seconds: orange from 100 ms without a look at the keys, red from
 *  500 ms. Late is a stretch of work between two waits for input, or a wait that ended that much past its time (the
 *  processor was given to others). The keys and knobs are read by the GUI loop between its stretches of work, so a
 *  key is acted on late by as much; a press that does nothing while the dot stays dark was not lost to a busy GUI.
 *
 *  Off by default: then nothing is read or drawn. On, three small files of /proc are read once a second, and the
 *  bars are drawn again only when one moves by a pixel or changes colour; not while the screen is dark or under
 *  the screensaver.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GUI thread, once the screen is built: follows the setting from then on */
void load_meter_init(void);

/* GUI loop: how long the keys could not be read, ms: a stretch of work between two waits for input, or how far a
 * wait ended past its time (nothing kept with the meter off) */
void load_meter_busy(uint32_t ms);

#ifdef __cplusplus
}
#endif
