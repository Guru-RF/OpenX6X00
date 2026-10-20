/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - PSK window (PSK31 and PSK63)
 *
 *  From SP2KMO's OpenX6100 psk31/olivia work (randal007 merge).
 */

#pragma once

#include "dialog.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_psk;

/* PSK63 (62.5 Bd) or PSK31; before the window opens or while it is open, not while transmitting */
void dialog_psk_set_mode(bool psk63);
bool dialog_psk_is_63(void);


#ifdef __cplusplus
}
#endif
