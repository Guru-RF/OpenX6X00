/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "knob_device.h"
#include "mfk_device.h"

#define VERSION "OpenX6X00 - v0.3.0"

/* The VOL knob and the MFK, opened at start */
extern rotary_t     *vol;
extern encoder_t    *mfk;

/* LVGL and the panel portrait (480x800, the walkie screen) or landscape; false without DRM */
bool main_display_portrait(bool on);
