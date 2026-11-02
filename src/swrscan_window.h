/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - SWR scan window
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "app_window.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_swrscan;

/* radio.c: one SWR reading from the base while scanning */
void dialog_swrscan_update(float vswr);
/* A scan running stops as Stop does, the screen saying "SWR scan stopped: no TX on <no>" (cfg_tx_refusal_text: the
 * radio moved where it may not transmit, #121). LVGL thread */
void dialog_swrscan_stop(const char *no);

#ifdef __cplusplus
}
#endif
