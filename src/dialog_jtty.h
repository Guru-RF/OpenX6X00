/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - JTTY window
 */

#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_jtty;

/* CapsLock+1 to 8 on a keyboard: macro n (1-8) sent, as its button. False: JTTY does not have the keyboard */
bool dialog_jtty_macro(int n);


#ifdef __cplusplus
}
#endif
