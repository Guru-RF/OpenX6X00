/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl/lvgl.h"

/* The box at the top left: VFO, mode, AGC, PRE, ATT, ATU and the status icons (deck: its two rows) */
lv_obj_t *info_init(lv_obj_t *parent);

/* The texts the box shows, for the other places that show them too */
const char *info_params_mode_label_get(void);   /* "USB-D", "CW-R", ... */
const char *info_params_agc(void);              /* "OFF" "SLOW" "FAST" "AUTO" */
const char *info_params_vfo_label_get(void);    /* "VFO-A", "SPL-B", "+600k" and "REV" with a TX shift ... */
/* A TX shift at work (split on, a shift on the band): "+600k", "-7.6M" on VFO A, "REV" on VFO B; NULL without */
const char *info_params_tx_shift_label(void);
/* In split the radio would transmit where it cannot (the TX frequency out of sight): shown in the error colour */
bool info_tx_blocked(void);

/* An app holds the mode (FT8, CW trainer, ...): the mode shown soft */
void info_lock_mode(bool lock);
