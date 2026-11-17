/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax charts at night, the setting (page 4 of the app: Night Auto / On / Off) and the answer for the
 *  drawing now (wefax/night.h). From OlivLoze's fork (F4OLI).
 */

#pragma once

#include "wefax/night.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dim the charts now: the setting, in Auto the sun at the station's locator, asked again once a minute */
bool wefax_night(void);

/* The button's label ("Night" over "Auto (on)"...) and the next setting */
const char *wefax_night_label(void);
void        wefax_night_next(void);

#ifdef __cplusplus
}
#endif
