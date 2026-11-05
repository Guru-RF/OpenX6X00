/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The Propagation app's world map: coastlines (the JS8 map's), the station in the middle, night
 *  shaded and the grey line, the dawn and dusk band, lit; markers and great-circle paths over it.
 *  One map at a time
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* w x h at the parent's 0, 0; centred on the station (lat/lon; NAN: none, the map on Greenwich) */
lv_obj_t *prop_map_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, double lat, double lon);

/* Day and night at t: the drawing starts over from the coastlines */
void prop_map_begin(time_t t);

/* Where lat/lon is on the map */
lv_point_t prop_map_xy(double lat, double lon);

void prop_map_square(double lat, double lon, lv_coord_t r, lv_color_t color, bool filled);
void prop_map_path(double lat1, double lon1, double lat2, double lon2, lv_color_t color, lv_coord_t width);
void prop_map_text(lv_coord_t x, lv_coord_t y, const lv_font_t *font, lv_color_t color, const char *text);

/* The station's own mark, then onto the screen */
void prop_map_end(void);

#ifdef __cplusplus
}
#endif
