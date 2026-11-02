/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Olivia window
 *
 *  From SP2KMO's OpenX6100 psk31/olivia work (randal007 merge).
 */

#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_olivia;

/* The window's Mode button has stepped to a Contestia variant. */
bool dialog_olivia_is_contestia(void);


#ifdef __cplusplus
}
#endif
