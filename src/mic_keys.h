/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include <stdint.h>

/* A hand mic key as the base reports it (RADIO_HKEY_*), 0 when it is let go */
void hkey_put(uint32_t key);
