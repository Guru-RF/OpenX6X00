/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax tilt: a station's line rate and the radio's never quite agree, each row comes a
 *  little earlier or later than the one before and the chart leans. Found on a chart by the shear that makes its
 *  vertical lines (frame, meridians) sharpest; kept per station, as it is the station's (DWD about -185 on the
 *  radio of F4OLI, Northwood about -10), so Auto Plan's charts come in straight. From OlivLoze's fork (F4OLI).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tilt is 1/1000 px per row of a picture this wide (the app's view), as the Tilt button and Fix count it */
#define WEFAX_TILT_UNIT_W   750
#define WEFAX_TILT_MAX      400

/* The tilt that straightens img (w x h grey, as received), -WEFAX_TILT_MAX..WEFAX_TILT_MAX. False when there is
 * none to trust: no memory, too small a picture, or the best at the end of the range (nothing lines up) */
bool wefax_auto_tilt(const uint8_t *img, unsigned w, unsigned h, int *tilt);

/* A station's tilt (the median of its last charts); false when none is known */
bool wefax_station_tilt(const char *station, int *tilt);
/* One more chart of station measured */
void wefax_station_tilt_learn(const char *station, int tilt);

#ifdef __cplusplus
}
#endif
