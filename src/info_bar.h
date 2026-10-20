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
const char *info_params_vfo_label_get(void);    /* "VFO-A", "SPL-B", ... */

/* An app holds the mode (FT8, CW trainer, ...): the mode shown soft */
void info_lock_mode(bool lock);
