/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The 3D waterfall's close-up: a fine 2D waterfall of 2, 5 or 10 kHz around the passband's middle, at the top of
 *  the waterfall area (Settings > Display, Close-up; not on bases without IQ). Its rows come from the DSP
 *  (dsp.cpp: a narrow spectrum of its own), its picture is drawn into the 3D's buffer (waterfall_view.c)
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define CLOSEUP_BINS    256

/* DSP thread: the span to compute, Hz across the strip; 0 none (not shown) */
uint32_t closeup_span(void);

/* DSP thread: one row, n (CLOSEUP_BINS) dB bins, the lowest frequency first, over width_hz centred on center_hz (an
 * absolute frequency: the rows of a retune stay where they were received); pb_half_hz the passband's half width
 * around that centre */
void closeup_data(const float *db, uint16_t n, uint32_t width_hz, int64_t center_hz, int32_t pb_half_hz);

/* LVGL thread */
void       closeup_init(lv_obj_t *parent);      /* its labels, children of the waterfall */
lv_coord_t closeup_height(lv_coord_t wf_height); /* the strip's height in the waterfall area; 0 when not shown */
bool       closeup_new_rows(void);              /* rows came since the last paint */

/* LVGL thread: the strip into the top strip_h pixels of the 3D's buffer: portrait as wfp (x at column WIDTH - 1 -
 * x, stride pixels a column), else landscape rows of WIDTH; whole when fresh (the buffer was drawn over), else
 * only when rows came. Hides its labels with strip_h 0 */
void closeup_paint(uint32_t *buf, bool portrait, int32_t stride, int32_t strip_h, bool fresh);
