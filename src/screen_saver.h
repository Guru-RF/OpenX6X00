/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the screensavers (screen_saver.c): over the whole screen while the radio is not used. screen_light.c
 *  shows and takes them away, when screen_idle.c says
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SCREEN_SAVER_* (screen_idle.h) over everything until screen_saver_hide(), LVGL's invalidation off meanwhile
 * (screen_light.c): only what the screensaver changes is drawn */
void screen_saver_show(uint8_t kind);

void screen_saver_hide(void);

#ifdef __cplusplus
}
#endif
