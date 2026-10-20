/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - RX recorder window
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "app_window.h"

extern dialog_t *dialog_recorder;

/* recorder.c: recording started (true) or stopped; the buttons follow while the window is open */
void dialog_recorder_set_on(bool on);
