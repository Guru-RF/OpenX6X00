/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Wavelog's live link, the GUI's side (#245). What the radio is on, for the web server, which posts it to
 *  the owner's Wavelog (webserver wavelog_live.py), and a tune asked for by a click on a spot in Wavelog. Both come
 *  through the configurator's socket (cfg_socket.cpp: radio_get, tune). A tune sets the frequency and the mode,
 *  nothing else: it never keys the radio, and it is refused while the radio transmits.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A mode a spot is tuned in, by its name as the web server's MODES have it ("LSB", "LSB-D", "USB", "USB-D", "CW",
 * "CWR", "AM", "NFM"; any case), as radio_mode_t; -1 for any other */
int wavelog_cat_mode(const char *name);

/* What the radio is on, one JSON line: {"t":"radio","hz":<the dial>,"tx_hz":<where it transmits: the other VFO in
 * split>,"mode":"USB-D","log_mode":"FT8","app":"FT8","pwr":5.0,"tx":0}. log_mode: the mode the quick QSO log would
 * give a QSO now (field_qso_mode), "" when it can't tell; pwr: the TX power in W, at most the digital apps' cap while
 * one is open. Any thread, as TCI reads the state (tci_server.c) */
int wavelog_cat_state(char *buf, size_t size);

/* Tune to hz in mode (wavelog_cat_mode's), as a click on a spot in Wavelog: NULL once tuned, else why not, a short
 * line. Refused while the radio transmits, where it doesn't receive now (a transverter's band with it off), and while
 * the frequency is locked (an app holds it, or the user's lock); the mode stays while an app holds it. Control comes
 * to the operator at the radio, as with a turn of the knob. The display says it: "Wavelog: 14.074.0 USB-D".
 * LVGL thread */
const char *wavelog_cat_tune(int32_t hz, int mode);

#ifdef __cplusplus
}
#endif
