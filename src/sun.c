/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Where the sun is (sun.h)
 */

#include "sun.h"

#include <math.h>

#define RAD (M_PI / 180.0)

static double wrap180(double a) {
    a = fmod(a + 180.0, 360.0);
    if (a < 0) a += 360.0;
    return a - 180.0;
}

void sun_subsolar(time_t t, double *lat, double *lon) {
    double d = (double)t / 86400.0 - 10957.5;               /* days from J2000.0 (2000-01-01 12:00 UTC) */
    double g = (357.529 + 0.98560028 * d) * RAD;            /* mean anomaly */
    double q = 280.459 + 0.98564736 * d;                    /* mean longitude */
    double l = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * RAD;   /* ecliptic longitude */
    double e = (23.439 - 0.00000036 * d) * RAD;             /* obliquity */
    double ra   = atan2(cos(e) * sin(l), cos(l)) / RAD;     /* right ascension, degrees */
    double gmst = 280.46061837 + 360.98564736629 * d;       /* Greenwich sidereal time, degrees */

    *lat = asin(sin(e) * sin(l)) / RAD;
    *lon = wrap180(ra - gmst);
}

double sun_elevation(double slat, double slon, double lat, double lon) {
    double c = sin(lat * RAD) * sin(slat * RAD) + cos(lat * RAD) * cos(slat * RAD) * cos((lon - slon) * RAD);

    return asin(fmax(-1.0, fmin(1.0, c))) / RAD;
}

sun_light_t sun_light(double elevation) {
    if (elevation < SUN_GREY_LOW) return SUN_NIGHT;
    if (elevation < SUN_GREY_HIGH) return SUN_GREY;
    return SUN_DAY;
}
