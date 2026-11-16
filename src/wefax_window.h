/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax app
 */

#pragma once

#include "app_window.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_wefax;

/* The app is open (the waterfall shows its tuning marks) */
bool dialog_wefax_is_active(void);

/* What the panel asks: the buttons' own handlers. LVGL thread; false when the app is not open. PLAN and UNPLAN: Auto
 * Plan on, and off (off when the app opens too, as its button) */
typedef enum { WEFAX_RC_AUTO, WEFAX_RC_CONT, WEFAX_RC_SAVE, WEFAX_RC_CLEAR, WEFAX_RC_PLAN, WEFAX_RC_UNPLAN } wefax_rc_t;

bool wefax_window_remote(wefax_rc_t what);

/* Auto Plan for the panel's state: on, and its next slot ("HH:MM station kHz", "" for none) into next. LVGL thread */
bool wefax_window_plan(char *next, size_t size);

#ifdef __cplusplus
}
#endif
