/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Map geometry for js8_map.c (no LVGL, so it can be checked on the PC):
 *
 *    gcc -DMAP_GEO_TEST -Isrc -o /tmp/mg src/map_geo.c -lm && /tmp/mg
 */

#include "map_geo.h"

#include <math.h>

#define SPAN_LAT (MAP_LAT_TOP - MAP_LAT_BOT)

double map_geo_dlon(double lon, double lon0) {
    return fmod(fmod(lon - lon0 + 180.0, 360.0) + 360.0, 360.0) - 180.0;
}

map_view_t map_geo_world(int w, int h, double lon0) {
    return (map_view_t){(MAP_LAT_TOP + MAP_LAT_BOT) / 2, lon0, 1, w, h};
}

map_view_t map_geo_zoom(int w, int h, int z, double lat, double lon) {
    if (z <= 1) return map_geo_world(w, h, lon);
    double half = SPAN_LAT / z / 2;
    lat         = fmax(-90.0 + half, fmin(90.0 - half, lat));
    return (map_view_t){lat, lon, z, w, h};
}

void map_geo_project(const map_view_t *v, double lat, double lon, double *x, double *y) {
    *x = (map_geo_dlon(lon, v->clon) * v->z / 360.0 + 0.5) * (v->w - 1);
    *y = (0.5 - (lat - v->clat) * v->z / SPAN_LAT) * (v->h - 1);
}

map_view_t map_geo_fit(int w, int h, double lon0, const double *lat, const double *lon, int n) {
    if (n < 2) return map_geo_world(w, h, lon0);   /* nothing to fit */
    double w0 = 180, e0 = -180, s0 = 90, n0 = -90;
    for (int i = 0; i < n; i++) {
        double d = map_geo_dlon(lon[i], lon0);
        w0 = fmin(w0, d);
        e0 = fmax(e0, d);
        s0 = fmin(s0, lat[i]);
        n0 = fmax(n0, lat[i]);
    }
    for (int z = MAP_ZOOM_MAX; z > 1; z /= 2) {
        /* The centre in steps of a 16th of the view: it keeps still while stations come and go */
        double     ql = 360.0 / z / 16, qa = SPAN_LAT / z / 16;
        map_view_t v  = map_geo_zoom(w, h, z, round((s0 + n0) / 2 / qa) * qa, lon0 + round((w0 + e0) / 2 / ql) * ql);
        int        i  = 0;
        for (; i < n; i++) {
            double x, y;
            map_geo_project(&v, lat[i], lon[i], &x, &y);
            if (x < w * 0.06 || x > w * 0.94 || y < h * 0.12 || y > h * 0.88) break;   /* clear of title and legend */
        }
        if (i == n) return v;
    }
    return map_geo_world(w, h, lon0);
}

double map_geo_bearing(double lat1, double lon1, double lat2, double lon2) {
    double r  = M_PI / 180.0;
    double dl = (lon2 - lon1) * r;
    double b  = atan2(sin(dl) * cos(lat2 * r), cos(lat1 * r) * sin(lat2 * r) - sin(lat1 * r) * cos(lat2 * r) * cos(dl));
    return fmod(b / r + 360.0, 360.0);
}

int map_geo_step(const double *dlon, int n, int cur, int dir) {
    if (n <= 0) return -1;
    if (cur >= 0 && cur < n) return ((cur + (dir > 0 ? 1 : -1)) % n + n) % n;
    int i = 0;
    while (i < n && dlon[i] < 0) i++;   /* the first one east */
    return dir > 0 ? i % n : (i + n - 1) % n;
}

#ifdef MAP_GEO_TEST
#include <assert.h>
#include <stdio.h>

static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

int main(void) {
    double x, y;

    /* World: the station's longitude in the middle, the antimeridian at the edges */
    map_view_t v = map_geo_world(771, 300, 8.0);
    map_geo_project(&v, MAP_LAT_TOP, 8.0, &x, &y);
    assert(near(x, 385, 0.01) && near(y, 0, 0.01));
    map_geo_project(&v, MAP_LAT_BOT, 8.0 + 179.999, &x, &y);
    assert(near(x, 770, 0.1) && near(y, 299, 0.01));
    assert(near(map_geo_dlon(-170, 170), 20, 1e-9) && near(map_geo_dlon(170, -170), -20, 1e-9));

    /* Zoom: the centre stays put, a degree is z times wider; never past the poles */
    v = map_geo_zoom(771, 300, 4, 45, 8);
    map_geo_project(&v, 45, 8 + 360.0 / 4 / 2, &x, &y);
    assert(near(x, 770, 0.01) && near(y, 149.5, 0.01));
    v = map_geo_zoom(771, 300, 2, 85, 8);
    assert(near(v.clat, 90 - 35, 1e-9));

    /* Fit: Europe from JN35 is zoomed in and every point is inside; one far point gives the world */
    double lat[] = {45.5, 52.5, 41.5, 60.5}, lon[] = {9, 13, 2, 25};
    v = map_geo_fit(771, 300, 9, lat, lon, 4);
    assert(v.z >= 4);
    for (int i = 0; i < 4; i++) {
        map_geo_project(&v, lat[i], lon[i], &x, &y);
        assert(x > 0 && x < 770 && y > 0 && y < 299);
    }
    double lat2[] = {45.5, -33.9}, lon2[] = {9, 151.2};
    assert(map_geo_fit(771, 300, 9, lat2, lon2, 2).z == 1);

    /* Bearing: north, east along the equator, Turin to New York about 297 */
    assert(near(map_geo_bearing(0, 0, 10, 0), 0, 1e-9));
    assert(near(map_geo_bearing(0, 0, 0, 10), 90, 1e-9));
    assert(near(map_geo_bearing(45.07, 7.69, 40.71, -74.0), 297, 1));

    /* Knob order: from none to the nearest east or west, then round */
    double dl[] = {-50, -3, 4, 90};
    assert(map_geo_step(dl, 4, -1, 1) == 2 && map_geo_step(dl, 4, -1, -1) == 1);
    assert(map_geo_step(dl, 4, 3, 1) == 0 && map_geo_step(dl, 4, 0, -1) == 3);
    double east[] = {5, 6};
    assert(map_geo_step(east, 2, -1, -1) == 1 && map_geo_step(dl, 0, -1, 1) == -1);

    puts("map_geo ok");
    return 0;
}
#endif
