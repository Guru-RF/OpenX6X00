/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Station callsign and QTH locator: on-screen keyboard setup and the
 *  checked save (Settings -> General, QTH dialog).
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prepare the textarea_window text for editing (chars, length, current value) */
void station_callsign_edit(lv_obj_t *text);
void station_qth_edit(lv_obj_t *text);

/* Check and store; false (with a message) if the value is not usable */
bool station_callsign_save(const char *callsign);
bool station_qth_save(const char *qth);

/* The locator to operate with: the 6 characters from a current GPS fix,
 * else the saved QTH (params.qth.x itself, never overwritten by the GPS). */
const char *station_qth(void);

#ifdef __cplusplus
}
#endif
