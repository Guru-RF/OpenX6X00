/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The ITU region of a position, for the band plan's Auto: the lines A, B and C of the Radio Regulations
 *  (Article 5, 5.3 to 5.9) and the countries 5.5 and 5.9 put whole in Region 1 or 3.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 1, 2 or 3 for lat, lon in degrees (north and east positive). Exact at the lines; the borders of the countries
 * that move a region are within a degree or so. Any thread */
int itu_region(double lat, double lon);

#ifdef __cplusplus
}
#endif
