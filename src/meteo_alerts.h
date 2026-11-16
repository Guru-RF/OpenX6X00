/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Weather warnings as notices on any screen (meteo_alerts.c): with APP > Weather > Notices on (off by
 *  default), every 30 minutes the warnings where the radio is; a new one for the place itself in the message box,
 *  only while receiving with the screen lit, not while APP > Weather is open.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

/* Its timer, the warnings shown before read back (/mnt/meteo/seen). LVGL thread */
void meteo_alerts_init(void);

/* The app's answer: the warnings it shows count as seen, no notice for them afterwards. LVGL thread */
void meteo_alerts_seen(const struct cJSON *data);

#ifdef __cplusplus
}
#endif
