/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 app
 */

#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_js8; /* the app, opened from APP */


/* The call of the station the list selection is on, false if none (the UI
 * test harness reads it). */
bool dialog_js8_selected_call(char *call, unsigned len); /* len: size of call */

#ifdef __cplusplus
}
#endif
