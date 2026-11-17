/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax charts at night (night.h).
 */

#include "night.h"

#include "../qth/qth.h"
#include "../sun.h"

bool wefax_night_at(int mode, const char *qth, time_t now)
{
    double slat, slon, lat, lon;

    if (mode == WEFAX_NIGHT_ON)
        return true;
    if (mode != WEFAX_NIGHT_AUTO || !qth || !qth_grid_check(qth) || now < WEFAX_NIGHT_CLOCK_SET)
        return false;
    qth_str_to_pos(qth, &lat, &lon);
    sun_subsolar(now, &slat, &slon);
    return sun_elevation(slat, slon, lat, lon) < SUN_GREY_LOW;
}
