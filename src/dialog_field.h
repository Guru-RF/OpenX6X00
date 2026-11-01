/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: a full-screen interface for portable activations
 *  (SOTA, POTA): the reference where you are, the activation with its log and spot,
 *  the bands with the Reverse Beacon Network. Its state is on the SD card, so leaving
 *  it (GEN) or a restart finds everything as it was.
 */

#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "qso_log.h"

extern dialog_t *dialog_field;

/* Companion app: field_screen (text SOTA, POTA, BANDS, LOG, HOME), field_ref (text), field_start,
 * field_end, field_log (text "CALL|rst sent|rst received"), field_spot, field_update */
/* qso_log (any app or none): text as field_log, into the day's log; false: not logged */
bool dialog_field_qso_log(const char *text);
/* A QSO of FT8, FT4 or JS8 into the day's log */
void dialog_field_day_log(const qso_log_record_t *r);

/* Wavelog live upload (x6100-wavelog): a first try a minute after the start, then after each QSO */
void dialog_field_wavelog_start(void);

/* Shared with the Bands app, band a field_band_of() index. Test beacon: TEST DE <call> in CW on band, the one chosen on
 * the screen; on the radio's own band where it is, on another at the band's CW frequency (dialog_field_tune_band's)
 * and back where the radio was once it is sent. Again while it is on: stops it, and back; tuned, Use band or another
 * mode meanwhile: stops it, the radio left as it was put. Refused, the screen saying why: no callsign, not in CW or
 * CW-R, MSG > CW sending or a beacon on, the radio in TX, the frequency, band or mode locked, the radio not
 * transmitting on that band (split, a TX shift, receive only) */
void dialog_field_test_beacon(int band);
/* Use band: a typical frequency of the band; a test on stops there */
void dialog_field_tune_band(int band);

#ifdef __cplusplus
}
#endif
