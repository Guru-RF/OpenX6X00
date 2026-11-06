/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The Propagation app's world map (prop_map.h)
 */

#include "prop_map.h"

#include "board.h"
#include "map_geo.h"
#include "sun.h"
#include "theme.h"
#include "world_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GC_STEPS 64

static struct {
    lv_obj_t   *canvas;
    lv_color_t *buf, *base;     /* what is shown; the coastlines it starts from */
    map_view_t  view;
    double      lat, lon;
} m;

lv_point_t prop_map_xy(double lat, double lon) {
    double x, y;

    map_geo_project(&m.view, lat, lon, &x, &y);
    return (lv_point_t){(lv_coord_t)fmax(-30000, fmin(30000, x)), (lv_coord_t)fmax(-30000, fmin(30000, y))};
}

/* A polyline, broken where it crosses the edge (a jump of half the map) */
static void polyline(lv_point_t *p, int n, const lv_draw_line_dsc_t *dsc) {
    int start = 0;

    for (int i = 1; i <= n; i++) {
        if (i == n || abs(p[i].x - p[i - 1].x) > m.view.w / 2) {
            if (i - start >= 2) lv_canvas_draw_line(m.canvas, p + start, (uint32_t)(i - start), dsc);
            start = i;
        }
    }
}

static void draw_base(void) {
    lv_draw_line_dsc_t dsc;

    lv_canvas_fill_bg(m.canvas, TC(bg), LV_OPA_COVER);

    lv_draw_line_dsc_init(&dsc);
    dsc.color = TC(text_faint);
    dsc.width = 1;
    dsc.opa   = LV_OPA_50;
    for (int lat = -60; lat <= 60; lat += 30) {
        lv_point_t p[2] = {{0, prop_map_xy(lat, 0).y}, {(lv_coord_t)(m.view.w - 1), prop_map_xy(lat, 0).y}};
        lv_canvas_draw_line(m.canvas, p, 2, &dsc);
    }
    for (int lon = -180; lon < 180; lon += 30) {
        lv_coord_t x    = prop_map_xy(0, lon).x;
        lv_point_t p[2] = {{x, 0}, {x, (lv_coord_t)(m.view.h - 1)}};
        lv_canvas_draw_line(m.canvas, p, 2, &dsc);
    }

    dsc.color = TC(text_dim);
    dsc.opa   = LV_OPA_COVER;
    for (int k = 0; k < WORLD_MAP_RINGS; k++) {
        int        a = world_map_rings[k], n = world_map_rings[k + 1] - a;
        lv_point_t p[n + 1];

        for (int i = 0; i <= n; i++) {   /* closed: back to the first point */
            const int16_t *v = world_map_pts[a + (i % n)];
            p[i]             = prop_map_xy(v[1] / 10.0, v[0] / 10.0);
        }
        polyline(p, n + 1, &dsc);
    }
    memcpy(m.base, m.buf, sizeof(lv_color_t) * m.view.w * m.view.h);
}

static void delete_cb(lv_event_t *e) {
    if (m.canvas != lv_event_get_target(e)) return;
    free(m.buf);
    m.buf = m.base = NULL;
    m.canvas       = NULL;
}

lv_obj_t *prop_map_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, double lat, double lon) {
    lv_color_t *buf = malloc(2 * sizeof(lv_color_t) * w * h);   /* the canvas, then the base */

    if (!buf) return NULL;
    m.buf    = buf;
    m.base   = buf + w * h;
    m.lat    = lat;
    m.lon    = lon;
    m.view   = map_geo_world(w, h, isnan(lon) ? 0.0 : round(lon / 15.0) * 15.0);   /* the station near the middle */
    m.canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(m.canvas, m.buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(m.canvas, 0, 0);
    lv_obj_add_event_cb(m.canvas, delete_cb, LV_EVENT_DELETE, NULL);
    draw_base();
    return m.canvas;
}

/* Night darkened, the grey line lit, a pixel at a time: the sun's height from the point under it
 * is sin(lat) sin(slat) + cos(lat) cos(slat) cos(lon - slon), a row and a column factor */
void prop_map_begin(time_t t) {
    if (!m.canvas) return;

    int    w = m.view.w, h = m.view.h;
    double slat, slon, r = M_PI / 180.0;
    double grey_lo = sin(SUN_GREY_LOW * r), grey_hi = sin(SUN_GREY_HIGH * r);
    static float cos_dl[BOARD_DISP_W];      /* the map is never wider than the panel */

    sun_subsolar(t, &slat, &slon);
    for (int x = 0; x < w && x < BOARD_DISP_W; x++) {
        double lon = m.view.clon + ((double)x / (w - 1) - 0.5) * 360.0;
        cos_dl[x]  = (float)cos((lon - slon) * r);
    }

    lv_color_t night = lv_color_black(), grey = TC(warn), day = TC(accent);

    for (int y = 0; y < h; y++) {
        double      lat = m.view.clat + (0.5 - (double)y / (h - 1)) * (MAP_LAT_TOP - MAP_LAT_BOT);
        float       a   = (float)(sin(lat * r) * sin(slat * r)), b = (float)(cos(lat * r) * cos(slat * r));
        lv_color_t *src = m.base + y * w, *dst = m.buf + y * w;

        for (int x = 0; x < w; x++) {
            float e = a + b * cos_dl[x];

            /* Day a light wash, night the coastlines dimmed: on a dark theme the night side has
             * little to darken */
            if (e < grey_lo) dst[x] = lv_color_mix(night, src[x], LV_OPA_60);
            else if (e < grey_hi) dst[x] = lv_color_mix(grey, src[x], LV_OPA_30);
            else dst[x] = lv_color_mix(day, src[x], LV_OPA_10);
        }
    }
}

void prop_map_square(double lat, double lon, lv_coord_t r, lv_color_t color, bool filled) {
    lv_draw_rect_dsc_t dsc;
    lv_point_t         c = prop_map_xy(lat, lon);

    lv_draw_rect_dsc_init(&dsc);
    if (filled) {
        dsc.bg_color = color;
    } else {
        dsc.bg_opa       = LV_OPA_TRANSP;
        dsc.border_color = color;
        dsc.border_width = r > 4 ? 2 : 1;
    }
    lv_canvas_draw_rect(m.canvas, c.x - r, c.y - r, 2 * r + 1, 2 * r + 1, &dsc);
}

void prop_map_path(double lat1, double lon1, double lat2, double lon2, lv_color_t color, lv_coord_t width) {
    lv_draw_line_dsc_t dsc;
    double             r    = M_PI / 180.0;
    double             a[3] = {cos(lat1 * r) * cos(lon1 * r), cos(lat1 * r) * sin(lon1 * r), sin(lat1 * r)};
    double             b[3] = {cos(lat2 * r) * cos(lon2 * r), cos(lat2 * r) * sin(lon2 * r), sin(lat2 * r)};
    double             d    = acos(fmax(-1.0, fmin(1.0, a[0] * b[0] + a[1] * b[1] + a[2] * b[2])));
    lv_point_t         p[GC_STEPS + 1];

    for (int i = 0; i <= GC_STEPS; i++) {
        double t  = (double)i / GC_STEPS;
        double ka = d < 1e-6 ? 1.0 - t : sin((1.0 - t) * d) / sin(d);
        double kb = d < 1e-6 ? t : sin(t * d) / sin(d);
        double x = ka * a[0] + kb * b[0], y = ka * a[1] + kb * b[1], z = ka * a[2] + kb * b[2];

        p[i] = prop_map_xy(atan2(z, sqrt(x * x + y * y)) / r, atan2(y, x) / r);
    }
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    polyline(p, GC_STEPS + 1, &dsc);
}

void prop_map_text(lv_coord_t x, lv_coord_t y, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_draw_label_dsc_t dsc;
    lv_point_t          size;

    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    x = LV_CLAMP(1, x, m.view.w - size.x - 2);
    y = LV_CLAMP(1, y, m.view.h - size.y - 1);
    lv_draw_label_dsc_init(&dsc);
    dsc.font  = font;
    dsc.color = color;
    lv_canvas_draw_text(m.canvas, x, y, size.x + 2, &dsc, text);
}

void prop_map_end(void) {
    if (!m.canvas) return;
    if (isnan(m.lat)) {
        lv_obj_invalidate(m.canvas);
        return;
    }
    prop_map_square(m.lat, m.lon, 4, TC(accent), true);
    prop_map_square(m.lat, m.lon, 7, TC(accent), false);
    lv_obj_invalidate(m.canvas);
}
