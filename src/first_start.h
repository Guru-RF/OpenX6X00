/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the first-start guide (first_start.c): WiFi, the callsign and locator, the clock and its time zone,
 *  then where the manual and the web configurator are. When it comes up by itself: first_start_rule.h
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "app_window.h"

/* The window: opened as an app (ACTION_APP_FIRST_START), from Settings > General > STATION too */
extern dialog_t *dialog_first_start;

/* At the start, once the screen and the radio are up: the guide a little later when it is due */
void first_start_init(void);
