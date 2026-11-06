/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Where the sun is: the point under it and its height over the horizon anywhere, for day and
 *  night on the maps (the grey line) and "by day here" in the band forecast. Accurate to a
 *  fraction of a degree (the Astronomical Almanac's low-precision formulas), plenty for a map
 */

#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The point the sun is overhead at time t: lat -23.4..23.4, lon -180..180 */
void sun_subsolar(time_t t, double *lat, double *lon);

/* The sun's height over the horizon at lat/lon (degrees, negative under it) for the subsolar
 * point slat/slon */
double sun_elevation(double slat, double slon, double lat, double lon);

/* Day, twilight (the grey line) and night by elevation: civil twilight's 6 degrees, and the
 * last few degrees above the horizon, when the D layer is still thin */
#define SUN_GREY_LOW   (-6.0)
#define SUN_GREY_HIGH  3.0

typedef enum { SUN_NIGHT, SUN_GREY, SUN_DAY } sun_light_t;

sun_light_t sun_light(double elevation);

#ifdef __cplusplus
}
#endif
