/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Maidenhead locators (qth.h)
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 *
 *  Each pair of characters splits the square above it: field 20x10 degrees (A-R), square 2x1 (0-9),
 *  subsquare 5'x2.5' (a-x), extended square 30"x15" (0-9). Longitude comes first in every pair.
 */

#include "qth.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#define EARTH_KM    6371.0

static bool in(char c, char lo, char hi) {
    return c >= lo && c <= hi;
}

bool qth_grid_check(const char *grid) {
    size_t len = strlen(grid);

    if (len != 2 && len != 4 && len != 6 && len != 8)
        return false;

    for (size_t i = 0; i < len; i++) {
        char c = grid[i];

        switch (i / 2) {
            case 0:  if (!in(toupper(c), 'A', 'R')) return false; break;
            case 2:  if (!in(toupper(c), 'A', 'X')) return false; break;
            default: if (!in(c, '0', '9')) return false; break;
        }
    }
    return true;
}

void qth_str_to_pos(const char *grid, double *lat_deg, double *lon_deg) {
    size_t  len = strlen(grid);
    double  lon = -180.0 + (toupper(grid[0]) - 'A') * 20.0;
    double  lat = -90.0 + (toupper(grid[1]) - 'A') * 10.0;
    double  lon_step = 20.0, lat_step = 10.0;

    if (len >= 4) {
        lon += (grid[2] - '0') * 2.0;
        lat += (grid[3] - '0') * 1.0;
        lon_step = 2.0;
        lat_step = 1.0;
    }
    if (len >= 6) {
        lon += (toupper(grid[4]) - 'A') * 5.0 / 60.0;
        lat += (toupper(grid[5]) - 'A') * 2.5 / 60.0;
        lon_step = 5.0 / 60.0;
        lat_step = 2.5 / 60.0;
    }
    if (len >= 8) {
        lon += (grid[6] - '0') * 5.0 / 600.0;
        lat += (grid[7] - '0') * 2.5 / 600.0;
        lon_step = 5.0 / 600.0;
        lat_step = 2.5 / 600.0;
    }

    /* Odd or longer lengths stay on the south-west corner, as they always did */
    if (len == 2 || len == 4 || len == 6 || len == 8) {
        lon += lon_step / 2;
        lat += lat_step / 2;
    }

    *lon_deg = lon;
    *lat_deg = lat;
}

/* Takes the index of the cell of size step that holds *rest and leaves the offset inside it */
static int cell(double *rest, double step) {
    int n = (int)(*rest / step);

    *rest -= n * step;
    return n;
}

/*
 * Out-of-range input is not rejected: the clamps keep north and east inside RR99xx99, south and
 * west give characters below 'A'/'0'. Callers only pass GPS fixes and saved positions.
 */
void qth_pos_to_str(double lat, double lon, char *buf) {
    int n;

    lon = fmin(lon, 179.99999) + 180.0;
    lat = fmin(lat, 89.99999) + 90.0;

    buf[0] = 'A' + cell(&lon, 20.0);
    buf[2] = '0' + cell(&lon, 2.0);
    lon *= 60.0;
    buf[4] = 'a' + cell(&lon, 5.0);
    lon *= 60.0;
    n = cell(&lon, 30.0);
    buf[6] = '0' + (n > 9 ? 9 : n);

    buf[1] = 'A' + cell(&lat, 10.0);
    buf[3] = '0' + cell(&lat, 1.0);
    lat *= 60.0;
    buf[5] = 'a' + cell(&lat, 2.5);
    lat *= 60.0;
    n = cell(&lat, 15.0);
    buf[7] = '0' + (n > 9 ? 9 : n);

    buf[8] = '\0';
}

static double rad(double deg) {
    return deg * M_PI / 180.0;
}

/* Haversine on a spherical Earth */
double qth_pos_dist(const double lat1_deg, const double lon1_deg, const double lat2_deg, const double lon2_deg) {
    double lat1 = rad(lat1_deg), lat2 = rad(lat2_deg);
    double s_lat = sin((lat1 - lat2) / 2.0);
    double s_lon = sin((rad(lon1_deg) - rad(lon2_deg)) / 2.0);
    double h = s_lat * s_lat + cos(lat1) * cos(lat2) * s_lon * s_lon;

    return 2.0 * atan2(sqrt(h), sqrt(1.0 - h)) * EARTH_KM;
}
