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

/* The TX views: the panel under the frequency strip, the strip over the digital apps.
 * Returns the object that takes the TX and RX events */
lv_obj_t *tx_info_init(lv_obj_t *parent);

/* A reading from the base (any thread): power in W, SWR, ALC as the base sends it */
void tx_info_update(float pwr, float swr, float alc);

/* The last power and SWR */
void tx_info_get(float *pwr, float *swr);

/* The length of the transmission starting, seconds (a slot: FT8 15, JS8 by its speed, WSPR 120): the
 * strip counts up to it, "5 / 15 s". 0 (the TX player at the end): the time alone. Any thread. */
void tx_info_set_length(float seconds);

/* The last readings if there is a new one since *seen (then *seen follows); any pointer may be NULL */
bool tx_info_refresh(uint8_t *seen, float *alc, float *pwr, float *swr);
