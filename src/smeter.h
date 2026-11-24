/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdint.h>
#include "lvgl/lvgl.h"

/* The S scale in dB: 6 dB an S unit, S9 at -73 dBm, then dB over S9 */
#define S_MIN   (-127)
#define S1      (-121)
#define S2      (S1 + 6)
#define S3      (S1 + 12)
#define S4      (S1 + 18)
#define S5      (S1 + 24)
#define S6      (S1 + 30)
#define S7      (S1 + 36)
#define S8      (S1 + 42)
#define S9      (S1 + 48)
#define S9_20   (S9 + 20)
#define S9_40   (S9 + 40)

/* The S-meter box: bars, needle or the deck scales, by theme and Settings */
lv_obj_t *meter_init(lv_obj_t *parent);

/* From the DSP: a new reading in dB and its smoothing (0 none, towards 1 slower) */
void meter_update(float db, float beta);

/* From the DSP: the noise floor in the filter, in dB; the bars under it stay grey */
void meter_set_noise(float db);

/* New TX readings (tx_meters.c, LVGL thread): the box shows them while transmitting with no TX panel */
void meter_tx_redraw(void);

/* The needle dial on any box (the walkie screen), at the current reading */
void meter_needle_draw(lv_obj_t *obj, lv_draw_ctx_t *draw_ctx);

/* The Internet radio's station on the box (#360, dialog_iradio.c): from begin to end, any thread, the box shows the
 * stream's audio level on a dB scale of full scale, marked Audio, in place of the receiver's signal, and at the end
 * the signal again at once. TX ends it too, and the TX readings show as ever. Feed, from the playing thread: a piece
 * of the samples just written, heard delay_ms from now (audio_listen_delay_ms; below 0: not heard). CAT, TCI, the
 * remote and the apps keep the receiver's reading (meter_get_db) */
void meter_audio_begin(void);
void meter_audio_feed(const int16_t *samples, size_t n, int delay_ms);
void meter_audio_end(void);

/* The station's scale: its dB of full scale where the S scale has S1, S5, S9, +20 and +40 (audio_meter.h). Every
 * style draws these in place of its S marks, those over S9 in the colour of its +20 and +40 */
typedef struct {
    int16_t     db;
    const char *t;
} meter_mark_t;

#define METER_AUDIO_MARKS 5
extern const meter_mark_t meter_audio_marks[METER_AUDIO_MARKS];

#ifdef BOARD_X6200
/* The supply as the TX power cap follows it (radio.c, LVGL thread): true on the battery alone. The X6200's attenuator
 * takes off more there, and the reading takes the band's figure for it back with ATT on (#125) */
void meter_set_battery(bool battery);

/* Any thread: the radio is on the battery alone, as the meter takes it */
bool meter_on_battery(void);
#endif

/* The last reading in dB, S1 to S9+40 */
int16_t meter_get_raw_db(void);

/* The same, unrounded (the front end measurement in Settings) */
float meter_get_db(void);

/* Counts the readings: a new one when it changed (the scanner and the beacons take each reading
 * once, and none from before a retune) */
uint32_t meter_seq(void);

/* x of db on a bar w wide with S9 at 70% (Codec's SIGNAL bar, Ops Room's hexagons): S1 at 0, S9+40 at w */
lv_coord_t meter_x70(float db, lv_coord_t w);
