/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#if HAVE_LIBGPS
#include <gps.h>          /* gpsd client library; a PC without it builds without GPS */
#endif
#include <stdbool.h>      /* for the C files that include only this */

typedef enum {
    GPS_STATUS_WAITING=0,
    GPS_STATUS_WORKING,
    GPS_STATUS_RESTARTING,
    GPS_STATUS_EXITED,
} gps_status_t;

void gps_init();

gps_status_t gps_status();

/* gpsd's last 2D or 3D position, any thread; age in seconds. False before the first fix */
bool gps_last_fix(double *lat, double *lon, int *age);
