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

/* Shared with the Bands app: TEST DE <call> in CW; a typical frequency of the band (cluster_band_of()) */
void dialog_field_test_beacon(void);
void dialog_field_tune_band(int band);

#ifdef __cplusplus
}
#endif
