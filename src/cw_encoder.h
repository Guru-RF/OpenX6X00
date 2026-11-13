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

#include "cw_beacon.h"

typedef enum {
    CW_ENCODER_IDLE = 0,
    CW_ENCODER_SEND,
    CW_ENCODER_BEACON,
    CW_ENCODER_BEACON_IDLE,
    CW_ENCODER_BEACON_WAIT      /* a beacon before its start time (cw_encoder_schedule), receiving */
} cw_encoder_state_t;

/* A TCI client's speed steps in its text (tci_server.c): from there on 5 WPM faster or slower, within
 * BOARD_KEY_SPEED_MIN to BOARD_KEY_SPEED_MAX, or back to the speed the send started at. No sign and no time of their
 * own. The radio's own texts never hold them */
#define CW_ENCODER_FASTER   '\x01'
#define CW_ENCODER_SLOWER   '\x02'
#define CW_ENCODER_SPEED    '\x03'
#define CW_ENCODER_STEP_WPM 5

/* The text keyed once, or again after each pause as a beacon. Its send's number: one more at every send started
 * (QRSS3's too), never 0; 0 when nothing started */
unsigned cw_encoder_send(const char *text, bool beacon);
void cw_encoder_stop();

/* The number of the send the keyer is on (a beacon's in its pauses too), 0 when it is idle; and the number of the last
 * send keyed to its end, not stopped (cw_encoder_stop, another send started, a key refused where the radio may not
 * transmit, its stops for the SWR or for leaving the band). A sender that kept its number knows from them whether the
 * keyer still sends its text, and whether all of it went. Any thread */
unsigned cw_encoder_sending(void);
unsigned cw_encoder_done(void);

/* The text in QRSS3 (cw_beacon.h), on the frequency the radio is on: every element by our own timing of the key,
 * never down longer than a dash (9 s) and up with the break-in delay as set, so the radio receives in every gap.
 * Besides cw_encoder_stop() it stops by itself, the screen saying why: a change of frequency, mode, VFO, split, TX
 * shift or XIT, the radio in TX between elements (the mic's PTT, CAT, TCI, the Panel), the key held past its TX time
 * limit (radio_set_morse_key_limit). False: not started. LVGL thread */
bool cw_encoder_send_qrss3(const char *text, bool beacon);

/* A QRSS3 send or beacon is on, its pause too. Any thread */
bool cw_encoder_qrss3(void);

/* Seconds left of the QRSS3 send on the air, 0 none */
int cw_encoder_send_left(void);

/* The front PTT key, before it keys: a QRSS3 send or a beacon (either speed) stops. LVGL thread */
void cw_encoder_ptt_pressed(void);

/* The start and the end (cw_beacon.h) of the next beacon sent, that one only: a later one starts at once and runs
 * until stopped unless given its own. The screen says when a beacon is over by it. LVGL thread */
void cw_encoder_schedule(const bcn_sched_t *s);

/* Seconds until a beacon's first send (BEACON_WAIT), 0 none */
int cw_encoder_wait_left(void);

/* The sends of the beacon on the air done so far */
int cw_encoder_sends_done(void);

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
