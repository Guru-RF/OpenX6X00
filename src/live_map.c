/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a world map with points on it (live_map.h): the DMR app's Map. The land and the lines of a view are
 *  drawn once and kept under the canvas; each drawing copies them back and puts the points on top.
 *
 *  From condoruy's CX8ANM edition of OpenX6X00 (its live_map.c, 6b640aaf).
 */
#include "live_map.h"

#include "map_geo.h"
#include "styles.h"
#include "theme.h"
#include "world_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    lv_obj_t   *canvas;
    lv_color_t *base;        /* graticule and coastlines of base_view, copied under every redraw */
    map_view_t  base_view, view;
    lv_coord_t  w, h;
} m;

static lv_point_t project(double lat, double lon) {
    double x, y;
    map_geo_project(&m.view, lat, lon, &x, &y);
    return (lv_point_t){(lv_coord_t)fmax(-30000, fmin(30000, x)), (lv_coord_t)fmax(-30000, fmin(30000, y))};
}

static void draw_path(lv_point_t *p, int n, const lv_draw_line_dsc_t *dsc) {
    int start = 0;
    for (int i = 1; i <= n; i++) {
        if (i == n || abs(p[i].x - p[i - 1].x) > m.view.w * m.view.z / 2) {
            if (i - start >= 2) lv_canvas_draw_line(m.canvas, p + start, (uint32_t)(i - start), dsc);
            start = i;
        }
    }
}

static void base_layer(void) {
    lv_draw_line_dsc_t dsc;
    lv_canvas_fill_bg(m.canvas, TC(panel), LV_OPA_COVER);

    lv_draw_line_dsc_init(&dsc);
    dsc.color = TC(text_faint);
    dsc.width = 1;
    dsc.opa   = LV_OPA_50;
    int step  = m.view.z <= 2 ? 30 : m.view.z <= 4 ? 10 : 5;
    for (int lat = -90 + step; lat < 90; lat += step) {
        lv_coord_t y = project(lat, m.view.clon).y;
        if (y <= 0 || y >= m.view.h - 1) continue;
        lv_point_t p[2] = {{0, y}, {(lv_coord_t)(m.view.w - 1), y}};
        lv_canvas_draw_line(m.canvas, p, 2, &dsc);
    }
    for (int lon = -180; lon < 180; lon += step) {
        lv_coord_t x = project(0, lon).x;
        if (x < 0 || x >= m.view.w) continue;
        lv_point_t p[2] = {{x, 0}, {x, (lv_coord_t)(m.view.h - 1)}};
        lv_canvas_draw_line(m.canvas, p, 2, &dsc);
    }

    lv_draw_line_dsc_init(&dsc);
    dsc.color = TC(text_dim);
    dsc.width = 1;
    for (int k = 0; k < WORLD_MAP_RINGS; k++) {
        int        a = world_map_rings[k], n = world_map_rings[k + 1] - a;
        lv_point_t p[n + 1];
        for (int i = 0; i <= n; i++) {
            const int16_t *v = world_map_pts[a + (i % n)];
            p[i]             = project(v[1] / 10.0, v[0] / 10.0);
        }
        draw_path(p, n + 1, &dsc);
    }
}

static void dot(lv_point_t c, lv_coord_t r, lv_color_t color, lv_opa_t opa) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius   = LV_RADIUS_CIRCLE;
    dsc.bg_color = color;
    dsc.bg_opa   = opa;
    lv_canvas_draw_rect(m.canvas, c.x - r, c.y - r, 2 * r + 1, 2 * r + 1, &dsc);
}

static void ring(lv_point_t c, lv_coord_t r, lv_color_t color) {
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = 2;
    lv_canvas_draw_arc(m.canvas, c.x, c.y, r, 0, 360, &dsc);
}

static lv_coord_t text_w(const char *s, const lv_font_t *font) {
    lv_point_t size;
    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void text(lv_coord_t x, lv_coord_t y, const lv_font_t *font, lv_color_t color, const char *s) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font  = font;
    dsc.color = color;
    lv_canvas_draw_text(m.canvas, x, y, text_w(s, font) + 2, &dsc, s);
}

static bool on_map(lv_point_t p) {
    return p.x >= 0 && p.y >= 0 && p.x < m.w && p.y < m.h;
}

static void box_draw(const char *box, lv_point_t hl, bool has_hl) {
    char         lines[6][64];
    int          n = 0;
    const char  *p = box;
    lv_coord_t   bw = 0;
    while (*p && n < 6) {
        size_t len = strcspn(p, "\n");
        snprintf(lines[n], sizeof(lines[0]), "%.*s", (int)len, p);
        lv_coord_t w = text_w(lines[n], n == 0 ? &sony_20 : &sony_16);
        if (w > bw) bw = w;
        n++;
        p += len;
        if (*p == '\n') p++;
    }
    if (!n) return;
    bw += 16;
    lv_coord_t bh = 8 + 22 + (n - 1) * 18;
    /* the corner away from the highlighted point */
    lv_coord_t x = (has_hl && hl.x > m.w / 2) ? 6 : m.w - bw - 6;
    lv_coord_t y = (has_hl && hl.y < m.h / 2) ? m.h - bh - 26 : 6;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color     = TC(panel);
    dsc.bg_opa       = LV_OPA_90;
    dsc.border_color = TC(warn);
    dsc.border_width = 2;
    dsc.radius       = 6;
    lv_canvas_draw_rect(m.canvas, x, y, bw, bh, &dsc);
    if (has_hl && on_map(hl)) {   /* a line from the window to the point */
        lv_draw_line_dsc_t l;
        lv_draw_line_dsc_init(&l);
        l.color         = TC(warn);
        l.width         = 1;
        l.opa           = LV_OPA_70;
        lv_point_t s[2] = {{(lv_coord_t)(x + (hl.x > x ? bw : 0)), (lv_coord_t)(y + (hl.y > y ? bh : 0))}, hl};
        lv_canvas_draw_line(m.canvas, s, 2, &l);
    }
    text(x + 8, y + 4, &sony_20, TC(warn), lines[0]);
    for (int i = 1; i < n; i++) text(x + 8, y + 8 + 22 + (i - 1) * 18 - 4, &sony_16, TC(text), lines[i]);
}

void live_map_draw(const live_map_pt_t *pts, int n, int zoom, double clat, double clon, const char *box,
                   const char *footer) {
    if (!m.canvas) return;
    int hl = -1;
    for (int i = 0; i < n; i++)
        if (pts[i].kind == LIVE_MAP_HIGHLIGHT) hl = i;
    if (zoom <= 1)
        m.view = map_geo_world(m.w, m.h, hl >= 0 ? pts[hl].lon : clon);
    else
        m.view = map_geo_zoom(m.w, m.h, zoom, hl >= 0 ? pts[hl].lat : clat, hl >= 0 ? pts[hl].lon : clon);

    size_t size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(m.w, m.h);
    void  *buf  = (void *)lv_canvas_get_img(m.canvas)->data;
    if (memcmp(&m.view, &m.base_view, sizeof(m.view))) {
        base_layer();
        memcpy(m.base, buf, size);
        m.base_view = m.view;
    } else {
        memcpy(buf, m.base, size);
    }

    for (int pass = 0; pass < 2; pass++)          /* the idle ones first, under the active ones */
        for (int i = 0; i < n; i++) {
            if (i == hl || (pts[i].kind == LIVE_MAP_ACTIVE) != pass) continue;
            lv_point_t c = project(pts[i].lat, pts[i].lon);
            if (!on_map(c)) continue;
            if (pass) dot(c, 3, TC(accent), LV_OPA_COVER);
            else dot(c, 2, TC(text_dim), LV_OPA_70);
        }
    lv_point_t hp = {0, 0};
    if (hl >= 0) {
        hp = project(pts[hl].lat, pts[hl].lon);
        if (on_map(hp)) {
            dot(hp, 5, TC(warn), LV_OPA_COVER);
            ring(hp, 10, TC(warn));
            ring(hp, 16, TC(warn));
            if (pts[hl].label[0]) text(hp.x + 18, hp.y - 10, &sony_18, TC(warn), pts[hl].label);
        }
    }
    if (footer && footer[0]) {
        lv_draw_rect_dsc_t band;
        lv_draw_rect_dsc_init(&band);
        band.bg_color = TC(panel);
        band.bg_opa   = LV_OPA_70;
        lv_canvas_draw_rect(m.canvas, 0, m.h - 22, m.w, 22, &band);
        text(6, m.h - 21, &sony_16, TC(text_dim), footer);
    }
    if (box && box[0]) box_draw(box, hp, hl >= 0);
    lv_obj_invalidate(m.canvas);
}

static void delete_cb(lv_event_t *e) {
    free(lv_event_get_user_data(e));
    if (m.canvas == lv_event_get_target(e)) m.canvas = NULL;
}

lv_obj_t *live_map_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    size_t   size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(w, h);
    uint8_t *buf  = malloc(2 * size);
    if (!buf) return NULL;
    m.w    = w;
    m.h    = h;
    m.base = (lv_color_t *)(buf + size);
    memset(&m.base_view, 0, sizeof(m.base_view));
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_event_cb(canvas, delete_cb, LV_EVENT_DELETE, buf);
    m.canvas = canvas;
    return canvas;
}
