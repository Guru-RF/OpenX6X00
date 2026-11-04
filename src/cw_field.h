/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CW from the keyboard on the main screen: in CW / CW-R, inside a CW segment of the band plan,
 *  with a USB or Bluetooth keyboard, typing opens a one-line field; Enter keys the line.
 */

#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void cw_field_init(lv_obj_t *parent);

/* A main screen key: true when the field took it */
bool cw_field_key(uint32_t key);

/* CapsLock+1 to 5 (screen_main.c): MSG > CW message n (1-5, in list order) keyed as if typed and sent with Enter,
 * where the field keys. False: not there, or no such message */
bool cw_field_message(int n);

/* CapsLock+Q (screen_main.c): the line typed is the QSO to log (cw_field_log_qso), cleared and not sent. False: not
 * where the field keys, or nothing typed */
bool cw_field_log_line(void);

/* A line to key after the queue, as typed plus Enter ({CALL} expanded) */
void cw_field_send_text(const char *text);

/* Log a QSO into the day's log: "CALL [rst sent [rst rcvd]]", the order of Field Ops' Log QSO; a report left out
 * is 599 in CW, 59 otherwise */
void cw_field_log_qso(const char *text);

/* The "Log QSO" button: a line to type the QSO in (keyboard or on-screen keys) */
void cw_field_log_open(void);

/* The field is on screen */
bool cw_field_visible(void);

#ifdef __cplusplus
}
#endif
