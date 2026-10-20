/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Maidenhead locators
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 *
 *  Positions are in degrees, north and east positive. Locators have 2, 4, 6 or 8 characters,
 *  letters in either case.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Centre of the square. Expects a checked locator: nothing is validated here */
void qth_str_to_pos(const char * qth, double *lat_deg, double *lon_deg);

/* Always writes 8 characters plus the terminator ("JN45ma54"), so qth needs 9 bytes */
void qth_pos_to_str(double lat_deg, double lon_deg, char * qth);

bool qth_grid_check(const char *grid);

/* Great-circle distance in km */
double qth_pos_dist(const double lat1_deg, const double lon1_deg, const double lat2_deg, const double lon2_deg);
