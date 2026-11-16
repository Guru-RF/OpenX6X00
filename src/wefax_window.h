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

/* What the panel asks: the buttons' own handlers. LVGL thread; false when the app is not open */
typedef enum { WEFAX_RC_AUTO, WEFAX_RC_CONT, WEFAX_RC_SAVE, WEFAX_RC_CLEAR } wefax_rc_t;

bool wefax_window_remote(wefax_rc_t what);

#ifdef __cplusplus
}
#endif
