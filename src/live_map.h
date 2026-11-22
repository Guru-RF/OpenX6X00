/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a world map with points on it: the DMR app's Map (dialog_dmr.c), the stations heard and the one
 *  talking. The coastlines of world_map.h and the projection of map_geo.c, as js8_map.c draws them; one map at a
 *  time.
 *
 *  From condoruy's CX8ANM edition of OpenX6X00 (its live_map.h, 6b640aaf).
 */
#pragma once

#include "lvgl/lvgl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LIVE_MAP_IDLE, LIVE_MAP_ACTIVE, LIVE_MAP_HIGHLIGHT };

typedef struct {
    float   lat, lon;
    uint8_t kind;        /* LIVE_MAP_*: a faint dot, a bright dot, a larger dot in two rings with its label */
    char    label[16];
} live_map_pt_t;

/* The canvas (w x h); deleting it frees the map */
lv_obj_t *live_map_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

/* Draw the points: zoom 1 the world, centred on the highlighted point's longitude, or on clon without one; 2, 4,
 * 8 ... around the highlighted point, or around clat/clon. box: the window over the map (lines separated by \n, the
 * first one larger), NULL none; it goes to the corner away from the highlighted point, with a line to it. footer: a
 * line at the bottom left, NULL none. LVGL thread */
void live_map_draw(const live_map_pt_t *pts, int n, int zoom, double clat, double clon, const char *box,
                   const char *footer);

#ifdef __cplusplus
}
#endif
