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

typedef enum {
    CW_ENCODER_IDLE = 0,
    CW_ENCODER_SEND,
    CW_ENCODER_BEACON,
    CW_ENCODER_BEACON_IDLE
} cw_encoder_state_t;

void cw_encoder_send(const char *text, bool beacon);
void cw_encoder_stop();

/* While set the key is never closed unless key train is on (the sidetone
 * without transmitting): the CW trainer must not transmit. */
void cw_encoder_require_train(bool on);

cw_encoder_state_t cw_encoder_state();

/* Characters of the text already keyed (0 when idle) */
int cw_encoder_sent();

/* Seconds left in a beacon pause */
int cw_encoder_pause_left();

/* The text with {CALL} as the station callsign in upper case (none set: left as is); a static buffer */
const char *cw_encoder_expand(const char *text);
