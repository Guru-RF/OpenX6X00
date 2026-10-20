/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "helpers.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <liquid/liquid.h>

typedef enum {
    RTTY_OFF = 0,
    RTTY_RX,
    RTTY_TX
} rtty_state_t;

void rtty_init();
void rtty_put_audio_samples(unsigned int n, float *samples);

void rtty_set_state(rtty_state_t state);

/* Tuning scope: the last RTTY_SCOPE_LEN samples through the mark (x) and space (y) filters,
 * the tones' offset from the centre in Hz; false when no symbol was decided lately */
#define RTTY_SCOPE_LEN 512
bool rtty_scope_get(float *x, float *y, float *offset);
rtty_state_t rtty_get_state();

float rtty_change_rate(int16_t df);
uint16_t rtty_change_shift(int16_t df);
uint16_t rtty_change_center(int16_t df);
bool rtty_change_reverse(int16_t df);

