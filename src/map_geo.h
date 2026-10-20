/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Map geometry for js8_map.c: the equirectangular view with zoom, the view
 *  that fits the stations, bearing and the knob's west-to-east order
 */

#pragma once

#define MAP_LAT_TOP  80.0    /* the world view leaves the polar caps out */
#define MAP_LAT_BOT  (-60.0)
#define MAP_ZOOM_MAX 16

typedef struct {
    double clat, clon;   /* centre */
    int    z;            /* 1 the world, 2, 4 ... MAP_ZOOM_MAX */
    int    w, h;         /* pixels */
} map_view_t;

/* Longitude east of lon0, -180..180 */
double map_geo_dlon(double lon, double lon0);

/* The world, centred on lon0 */
map_view_t map_geo_world(int w, int h, double lon0);

/* z times the world around lat/lon, kept inside the poles */
map_view_t map_geo_zoom(int w, int h, int z, double lat, double lon);

void map_geo_project(const map_view_t *v, double lat, double lon, double *x, double *y);

/* The closest view (lon0 at the centre of the world one) that shows the n points, clear of the edges */
map_view_t map_geo_fit(int w, int h, double lon0, const double *lat, const double *lon, int n);

/* Initial great-circle bearing from 1 to 2, degrees from north, 0..360 */
double map_geo_bearing(double lat1, double lon1, double lat2, double lon2);

/* One knob step through n stations sorted west to east (dlon: their map_geo_dlon from the station).
 * From none (cur < 0) the first step goes to the nearest one east (dir > 0) or west of the station */
int map_geo_step(const double *dlon, int n, int cur, int dir);
