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

/* A line to key after the queue, as typed plus Enter ({CALL} expanded) */
void cw_field_send_text(const char *text);

/* Log a QSO into the day's log: "CALL [rst sent [rst rcvd]]", 599 when left out */
void cw_field_log_qso(const char *text);

/* The "Log QSO" button: a line to type the QSO in (keyboard or on-screen keys) */
void cw_field_log_open(void);

/* The field is on screen */
bool cw_field_visible(void);

#ifdef __cplusplus
}
#endif
