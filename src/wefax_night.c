/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax charts at night, the setting (wefax_night.h).
 */

#include "wefax_night.h"

#include "cfg/cfg_api.h"
#include "station.h"

#include <time.h>

bool wefax_night(void)
{
    static time_t asked;
    static bool   night;
    int           mode = (int)param_i_get(cfg_wefax_night);
    time_t        now  = time(NULL);

    if (mode != WEFAX_NIGHT_AUTO)
        return wefax_night_at(mode, NULL, now);
    if (asked && now - asked < 60 && now >= asked)
        return night;
    asked = now;
    night = wefax_night_at(mode, station_qth(), now);
    return night;
}

const char *wefax_night_label(void)
{
    switch (param_i_get(cfg_wefax_night)) {
        case WEFAX_NIGHT_ON:  return "Night\nOn";
        case WEFAX_NIGHT_OFF: return "Night\nOff";
        default:              return wefax_night() ? "Night\nAuto (on)" : "Night\nAuto";
    }
}

void wefax_night_next(void)
{
    param_i_set(cfg_wefax_night, (param_i_get(cfg_wefax_night) + 1) % WEFAX_NIGHT_MODES);
}
