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


#ifdef __cplusplus
}
#endif
