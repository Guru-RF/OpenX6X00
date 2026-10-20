/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  JS8 map: the world with a path from the station to each QSO of the log,
 *  and the stations heard, kept up to date while it is open. The main knob
 *  zooms, the MFK selects a station and its press calls it. The legend and
 *  the label of the selected station (call, country, SNR, km, azimuth)
 *  follow the map of randal007's X6100 JS8 app.
 */

#define _GNU_SOURCE

#include "js8_map.h"

#include "dxcc.h"
#include "map_geo.h"
#include "qth/qth.h"
#include "styles.h"
#include "theme.h"
#include "world_map.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_QSOS   200
#define SPOTS_MAX  (HEARD_MAX + MAX_QSOS)
#define GC_STEPS   48
#define LOG_MAX    (512 * 1024)
#define HEARD_MAX  300
#define HEARD_S    (15 * 60)
#define RECENT_S   60        /* the last slots: bigger, ringed, labelled first */
#define REFRESH_MS 3000
#define ZOOM_STEPS 5         /* World, x2, x4, x8, x16; 0 is Auto */
#define TOP_H      30        /* title band */
#define BOT_H      30        /* legend band */

typedef struct {
    char call[16];
    char grid[8];
} qso_t;

typedef struct {
    js8_map_station_t s;
    double            lat, lon, dlon, km, az;
    const char       *country;
    bool              guessed;   /* no grid: the centre of its country */
    bool              worked;
    lv_point_t        p;
} spot_t;

static map_view_t view;   /* the one drawn now */

static struct {
    lv_obj_t      *canvas;
    lv_color_t    *base;      /* the graticule and coastlines of base_view, copied under every redraw */
    map_view_t     base_view;
    lv_timer_t    *timer;
    js8_map_fill_t fill;
    js8_map_pick_t pick;
    bool           log_spots;
    lv_coord_t     w, h;
    double         my_lat, my_lon;
    int            zoom;        /* 0 Auto, 1 the world, else x2^(zoom-1) */
    double         clat, clon;  /* centre of a manual zoom */
    char           sel[16];     /* the selected call, "" none */
    qso_t          log[MAX_QSOS];
    int            log_n;
    spot_t         spots[SPOTS_MAX];   /* west to east */
    int            n, heard;
} m;

static lv_point_t project(double lat, double lon) {
    double x, y;
    map_geo_project(&view, lat, lon, &x, &y);
    return (lv_point_t){(lv_coord_t)fmax(-30000, fmin(30000, x)), (lv_coord_t)fmax(-30000, fmin(30000, y))};
}

/* A polyline, broken where it goes over the edge (a jump of half the world) */
static void draw_path(lv_obj_t *canvas, lv_point_t *p, int n, const lv_draw_line_dsc_t *dsc) {
    int start = 0;
    for (int i = 1; i <= n; i++) {
        if (i == n || abs(p[i].x - p[i - 1].x) > view.w * view.z / 2) {
            if (i - start >= 2) lv_canvas_draw_line(canvas, p + start, (uint32_t)(i - start), dsc);
            start = i;
        }
    }
}

static void coastlines(lv_obj_t *canvas) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = TC(text_dim);
    dsc.width = 1;

    for (int k = 0; k < WORLD_MAP_RINGS; k++) {
        int        a = world_map_rings[k], n = world_map_rings[k + 1] - a;
        lv_point_t p[n + 1];
        for (int i = 0; i <= n; i++) {  /* closed: back to the first point */
            const int16_t *v = world_map_pts[a + (i % n)];
            p[i]             = project(v[1] / 10.0, v[0] / 10.0);
        }
        draw_path(canvas, p, n + 1, &dsc);
    }
}

/* Every 30 degrees on the world, closer when zoomed in */
static void graticule(lv_obj_t *canvas) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = TC(text_faint);
    dsc.width = 1;
    dsc.opa   = LV_OPA_50;

    int step = view.z <= 2 ? 30 : view.z <= 4 ? 10 : 5;
    for (int lat = -90 + step; lat < 90; lat += step) {
        lv_coord_t y = project(lat, view.clon).y;
        if (y <= 0 || y >= view.h - 1) continue;
        lv_point_t p[2] = {{0, y}, {(lv_coord_t)(view.w - 1), y}};
        lv_canvas_draw_line(canvas, p, 2, &dsc);
    }
    for (int lon = -180; lon < 180; lon += step) {
        lv_coord_t x = project(0, lon).x;
        if (x < 0 || x >= view.w) continue;
        lv_point_t p[2] = {{x, 0}, {x, (lv_coord_t)(view.h - 1)}};
        lv_canvas_draw_line(canvas, p, 2, &dsc);
    }
}

/* <name:len>value of an ADIF record, "" when missing */
static void adif_field(const char *rec, const char *end, const char *name, char *out, size_t size) {
    char tag[24];
    out[0] = 0;
    snprintf(tag, sizeof(tag), "<%s:", name);
    const char *t = strcasestr(rec, tag);
    if (!t || t >= end) return;
    size_t len = strtoul(t + strlen(tag), NULL, 10);
    const char *v = strchr(t, '>');
    if (!v || v >= end) return;
    v++;
    if (len >= size) len = size - 1;
    memcpy(out, v, len);
    out[len] = 0;
}

/* The calls of the log with a grid, the latest record of each */
static int read_qsos(const char *path, qso_t *q, int max) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > LOG_MAX ? size - LOG_MAX : 0;   /* a long log: its latest QSOs */
    char *buf = malloc((size_t)(size - from) + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    fseek(f, from, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)(size - from), f);
    fclose(f);
    buf[got] = 0;

    int   n   = 0;
    char *rec = buf;
    char *eor;
    while ((eor = strcasestr(rec, "<eor>"))) {
        qso_t e;
        adif_field(rec, eor, "call", e.call, sizeof(e.call));
        adif_field(rec, eor, "gridsquare", e.grid, sizeof(e.grid));
        rec = eor + 5;
        if (!e.call[0] || strlen(e.grid) < 4) continue;
        int i = 0;
        while (i < n && strcasecmp(q[i].call, e.call)) i++;
        if (i == n && n < max) n++;
        if (i < n) q[i] = e;
    }
    free(buf);
    return n;
}

static void gc_path(lv_obj_t *canvas, double lat1, double lon1, double lat2, double lon2, const lv_draw_line_dsc_t *dsc) {
    double     r = M_PI / 180.0;
    double     a[3] = {cos(lat1 * r) * cos(lon1 * r), cos(lat1 * r) * sin(lon1 * r), sin(lat1 * r)};
    double     b[3] = {cos(lat2 * r) * cos(lon2 * r), cos(lat2 * r) * sin(lon2 * r), sin(lat2 * r)};
    double     d    = acos(fmax(-1.0, fmin(1.0, a[0] * b[0] + a[1] * b[1] + a[2] * b[2])));
    lv_point_t p[GC_STEPS + 1];

    for (int i = 0; i <= GC_STEPS; i++) {
        double t = (double)i / GC_STEPS, ka, kb;
        if (d < 1e-6) {
            ka = 1.0 - t;
            kb = t;
        } else {
            ka = sin((1.0 - t) * d) / sin(d);
            kb = sin(t * d) / sin(d);
        }
        double x = ka * a[0] + kb * b[0], y = ka * a[1] + kb * b[1], z = ka * a[2] + kb * b[2];
        p[i]     = project(atan2(z, sqrt(x * x + y * y)) / r, atan2(y, x) / r);
    }
    draw_path(canvas, p, GC_STEPS + 1, dsc);
}

/* A square, filled or only its outline */
static void square(lv_obj_t *canvas, lv_point_t c, lv_coord_t r, lv_color_t color, bool filled) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    if (filled) {
        dsc.bg_color = color;
    } else {
        dsc.bg_opa       = LV_OPA_TRANSP;
        dsc.border_color = color;
        dsc.border_width = r > 5 ? 2 : 1;
    }
    lv_canvas_draw_rect(canvas, c.x - r, c.y - r, 2 * r + 1, 2 * r + 1, &dsc);
}

static lv_coord_t text_w(const char *s, const lv_font_t *font) {
    lv_point_t size;
    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void text(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, const lv_font_t *font, lv_color_t color, const char *s) {
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font  = font;
    label.color = color;
    lv_canvas_draw_text(canvas, x, y, text_w(s, font) + 2, &label, s);
}

/* ---- Stations ------------------------------------------------------------ */

static bool in_log(const char *call) {
    for (int i = 0; i < m.log_n; i++)
        if (!strcasecmp(m.log[i].call, call)) return true;
    return false;
}

/* Placed by its grid, or by its country when it sent none; left out when neither is known */
static void add_spot(const js8_map_station_t *s, bool worked) {
    spot_t *p = &m.spots[m.n];
    char    g4[5];
    snprintf(g4, sizeof(g4), "%s", s->grid);
    p->guessed = strlen(g4) < 4 || !qth_grid_check(g4);
    if (!p->guessed) qth_str_to_pos(s->grid, &p->lat, &p->lon);
    else if (!dxcc_pos(s->call, &p->lat, &p->lon)) return;

    p->s       = *s;
    p->worked  = worked;
    p->dlon    = map_geo_dlon(p->lon, m.my_lon);
    p->km      = qth_pos_dist(m.my_lat, m.my_lon, p->lat, p->lon);
    p->az      = map_geo_bearing(m.my_lat, m.my_lon, p->lat, p->lon);
    p->country = NULL;
    dxcc_lookup(s->call, &p->country, NULL);   /* cty.dat is read by then (decodes); else once, about 0.1 s */
    m.n++;
}

static int west_to_east(const void *a, const void *b) {
    const spot_t *x = a, *y = b;
    if (x->dlon != y->dlon) return x->dlon < y->dlon ? -1 : 1;
    return (x->lat < y->lat) - (x->lat > y->lat);   /* north first */
}

static void collect(void) {
    static js8_map_station_t st[HEARD_MAX];
    int                      n = m.fill ? m.fill(st, HEARD_MAX) : 0;

    m.n = 0;
    for (int i = 0; i < n; i++) add_spot(&st[i], in_log(st[i].call));
    m.heard = m.n;
    for (int i = 0; m.log_spots && i < m.log_n; i++) {   /* the log's, unless heard too */
        int k = 0;
        while (k < m.heard && strcasecmp(m.spots[k].s.call, m.log[i].call)) k++;
        if (k < m.heard) continue;
        js8_map_station_t s = {.snr = JS8_MAP_NO_SNR};
        snprintf(s.call, sizeof(s.call), "%s", m.log[i].call);
        snprintf(s.grid, sizeof(s.grid), "%s", m.log[i].grid);
        add_spot(&s, true);
    }
    qsort(m.spots, m.n, sizeof(m.spots[0]), west_to_east);
}

static int find_sel(void) {
    for (int i = 0; m.sel[0] && i < m.n; i++)
        if (!strcmp(m.spots[i].s.call, m.sel)) return i;
    return -1;
}

/* Auto: all the stations and ours. Zoomed: around the selected one when it gets near an edge */
static void choose_view(int sel) {
    if (m.zoom == 0) {
        static double lat[SPOTS_MAX + 1], lon[SPOTS_MAX + 1];
        lat[0] = m.my_lat;
        lon[0] = m.my_lon;
        for (int i = 0; i < m.n; i++) {
            lat[i + 1] = m.spots[i].lat;
            lon[i + 1] = m.spots[i].lon;
        }
        view = map_geo_fit(m.w, m.h, m.my_lon, lat, lon, m.n + 1);
        return;
    }
    view = map_geo_zoom(m.w, m.h, 1 << (m.zoom - 1), m.clat, m.clon);
    if (m.zoom == 1 || sel < 0) return;
    double x, y;
    map_geo_project(&view, m.spots[sel].lat, m.spots[sel].lon, &x, &y);
    if (x < m.w * 0.1 || x > m.w * 0.9 || y < m.h * 0.18 || y > m.h * 0.82) {
        m.clat = m.spots[sel].lat;
        m.clon = m.spots[sel].lon;
        view   = map_geo_zoom(m.w, m.h, 1 << (m.zoom - 1), m.clat, m.clon);
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static bool on_map(lv_point_t p) {
    return p.x >= 0 && p.x < m.w && p.y > TOP_H && p.y < m.h - BOT_H;
}

static void label_selected(lv_obj_t *canvas, const spot_t *s, time_t now) {
    char line[96];
    int  o = snprintf(line, sizeof(line), "%s", s->s.call);
    if (s->country) o += snprintf(line + o, sizeof(line) - o, "  %s", s->country);
    if (s->s.snr != JS8_MAP_NO_SNR) o += snprintf(line + o, sizeof(line) - o, "  %+d dB", s->s.snr);
    o += snprintf(line + o, sizeof(line) - o, "  %s%.0f km  az %.0f", s->guessed ? "~" : "", s->km, s->az);
    if (s->s.t) {
        long age = (long)(now - s->s.t);
        snprintf(line + o, sizeof(line) - o, age < 60 ? "  now" : "  %ld min", age / 60);
    }

    lv_coord_t bw = text_w(line, &sony_22) + 12, bh = lv_font_get_line_height(&sony_22) + 6;
    lv_coord_t x  = s->p.x - bw / 2;   /* over or under it */
    lv_coord_t y  = s->p.y - 14 - bh > TOP_H ? s->p.y - 14 - bh : s->p.y + 12;
    x             = LV_CLAMP(2, x, m.w - bw - 2);
    y             = LV_CLAMP(TOP_H + 2, y, m.h - BOT_H - bh - 2);

    lv_draw_rect_dsc_t box;
    lv_draw_rect_dsc_init(&box);
    box.bg_color     = TC(err_bg);
    box.border_color = TC(err);
    box.border_width = 1;
    box.radius       = 3;
    lv_canvas_draw_rect(canvas, x, y, bw, bh, &box);
    text(canvas, x + 6, y + 3, &sony_22, TC(text), line);
}

static void legend(lv_obj_t *canvas) {
    lv_coord_t x = 12, y = m.h - BOT_H / 2, ty = y - lv_font_get_line_height(&sony_20) / 2;
    for (int i = 0; i < 6; i++) {
        static const char *const name[] = {"heard", "hears me", "QSO", "by prefix", "selected", "me"};
        if (!m.fill && i != 2 && i != 4 && i != 5) continue;   /* the log map: QSOs only */
        lv_point_t p = {x, y};
        if (i == 2) {
            lv_draw_line_dsc_t dsc;
            lv_draw_line_dsc_init(&dsc);
            dsc.color       = TC(err);
            dsc.width       = 2;
            lv_point_t l[2] = {{x - 6, y}, {x + 6, y}};
            lv_canvas_draw_line(canvas, l, 2, &dsc);
        } else {
            static const bool filled[] = {true, true, false, false, false, true};
            lv_color_t        c[]      = {TC(ok), TC(info), TC(err), TC(ok), TC(err), TC(warn)};
            square(canvas, p, i == 4 ? 6 : 4, c[i], filled[i]);
        }
        text(canvas, x + 10, ty, &sony_20, TC(text_dim), name[i]);
        x += text_w(name[i], &sony_20) + 30;
    }
    const char *hint = m.pick ? "Main knob: zoom  MFK: select, press: QSO  ESC: close"
                              : "Main knob: zoom  MFK: select  ESC: close";
    if (x + text_w(hint, &sony_20) > m.w - 6) hint = "ESC: close";
    if (x + text_w(hint, &sony_20) > m.w - 6) return;   /* no room (VISION) */
    text(canvas, m.w - 6 - text_w(hint, &sony_20), ty, &sony_20, TC(text_dim), hint);
}

static void overlay(lv_obj_t *canvas, int sel) {
    time_t now = time(NULL);
    for (int i = 0; i < m.n; i++) m.spots[i].p = project(m.spots[i].lat, m.spots[i].lon);

    /* Paths: red to the stations worked, blue to those that heard us */
    lv_draw_line_dsc_t path;
    lv_draw_line_dsc_init(&path);
    path.width = 1;
    path.opa   = LV_OPA_80;
    for (int i = 0; i < m.n; i++) {
        const spot_t *s = &m.spots[i];
        if (!s->worked && !s->s.heard_me) continue;
        path.color = s->worked ? TC(err) : TC(info);
        gc_path(canvas, m.my_lat, m.my_lon, s->lat, s->lon, &path);
    }

    /* Squares, the recent ones over the others; hollow when placed by the country */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < m.n; i++) {
            const spot_t *s      = &m.spots[i];
            bool          recent = s->s.t && now - s->s.t < RECENT_S;
            if (recent != pass) continue;
            lv_color_t c = s->s.heard_me ? TC(info) : TC(ok);
            if (recent && !s->guessed) square(canvas, s->p, 5, TC(text), true);
            square(canvas, s->p, recent ? 4 : 3, c, !s->guessed);
        }

    /* Calls, the recent ones first, only where they don't overlap one already there;
     * "+2" when more stations share the spot (a grid square) */
    static lv_area_t placed[SPOTS_MAX];
    int              np = 0;
    for (int pass = 1; pass >= 0; pass--)
        for (int i = m.n - 1; i >= 0; i--) {
            const spot_t *s      = &m.spots[i];
            bool          recent = s->s.t && now - s->s.t < RECENT_S;
            if (recent != pass || i == sel || !on_map(s->p)) continue;
            char call[24];
            int  same = 0;
            for (int k = 0; k < m.n; k++)   /* ponytail: O(n^2), a few hundred stations at most */
                same += k != i && abs(m.spots[k].p.x - s->p.x) <= 2 && abs(m.spots[k].p.y - s->p.y) <= 2;
            snprintf(call, sizeof(call), same ? "%s +%d" : "%s", s->s.call, same);
            lv_coord_t w = text_w(call, &sony_18);
            lv_coord_t x = s->p.x + 8 + w < m.w ? s->p.x + 8 : s->p.x - 8 - w;
            lv_area_t  a = {x - 2, s->p.y - 11, x + w + 2, s->p.y + 11};
            bool       free_spot = a.y1 > TOP_H && a.y2 < m.h - BOT_H;
            for (int k = 0; k < np && free_spot; k++) free_spot = !_lv_area_is_on(&a, &placed[k]);
            if (!free_spot) continue;
            placed[np++] = a;
            text(canvas, x, s->p.y - 11, &sony_18, recent ? TC(text) : TC(text_dim), call);
        }
    square(canvas, project(m.my_lat, m.my_lon), 5, TC(warn), true);

    if (sel >= 0) {
        square(canvas, m.spots[sel].p, 8, TC(err), false);
        label_selected(canvas, &m.spots[sel], now);
    }

    /* Title and legend on bands of the panel colour, clear of the coastlines */
    lv_draw_rect_dsc_t band;
    lv_draw_rect_dsc_init(&band);
    band.bg_color = TC(panel);
    band.bg_opa   = LV_OPA_80;
    lv_canvas_draw_rect(canvas, 0, 0, m.w, TOP_H, &band);
    lv_canvas_draw_rect(canvas, 0, m.h - BOT_H, m.w, BOT_H, &band);

    char head[96];
    if (m.fill) {
        int hear_me = 0, dx = -1;
        for (int i = 0; i < m.n; i++) {
            if (!m.spots[i].s.t) continue;   /* the log's */
            hear_me += m.spots[i].s.heard_me;
            if (dx < 0 || m.spots[i].km > m.spots[dx].km) dx = i;
        }
        int o = snprintf(head, sizeof(head), "%d heard  %d hear me", m.heard, hear_me);
        if (dx >= 0) snprintf(head + o, sizeof(head) - o, "  DX %s %.0f km", m.spots[dx].s.call, m.spots[dx].km);
    } else {
        snprintf(head, sizeof(head), "QSO map - %d station%s", m.n, m.n == 1 ? "" : "s");
    }
    text(canvas, 8, 4, &sony_20, TC(text_head), head);

    char zoom[24];
    if (m.zoom) snprintf(zoom, sizeof(zoom), view.z == 1 ? "World" : "x%d", view.z);
    else snprintf(zoom, sizeof(zoom), view.z == 1 ? "Auto: world" : "Auto: x%d", view.z);
    text(canvas, m.w - 8 - text_w(zoom, &sony_20), 4, &sony_20, TC(text_dim), zoom);

    legend(canvas);
}

static void redraw(void) {
    if (!m.canvas) return;
    collect();
    int sel = find_sel();
    choose_view(sel);

    size_t size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(m.w, m.h);
    void  *buf  = (void *)lv_canvas_get_img(m.canvas)->data;
    if (memcmp(&view, &m.base_view, sizeof(view))) {   /* coastlines again only for a new view */
        lv_canvas_fill_bg(m.canvas, TC(panel), LV_OPA_COVER);
        graticule(m.canvas);
        coastlines(m.canvas);
        memcpy(m.base, buf, size);
        m.base_view = view;
    } else {
        memcpy(buf, m.base, size);
    }
    overlay(m.canvas, sel);
    lv_obj_invalidate(m.canvas);
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    redraw();
}

void js8_map_zoom(int32_t diff) {
    static uint32_t last;
    if (!m.canvas || !diff || lv_tick_elaps(last) < 150) return;   /* a level per flick of the knob */
    last  = lv_tick_get();
    int z = m.zoom + (diff > 0 ? 1 : -1);
    if (z < 0 || z > ZOOM_STEPS) return;
    m.zoom  = z;
    int sel = find_sel();   /* around the selected station, else ours */
    m.clat  = sel >= 0 ? m.spots[sel].lat : m.my_lat;
    m.clon  = sel >= 0 ? m.spots[sel].lon : m.my_lon;
    redraw();
}

static void js8_map_step(int dir) {
    if (!m.canvas) return;
    static double dlon[SPOTS_MAX];
    for (int i = 0; i < m.n; i++) dlon[i] = m.spots[i].dlon;
    int i = map_geo_step(dlon, m.n, find_sel(), dir);
    if (i < 0) return;
    snprintf(m.sel, sizeof(m.sel), "%s", m.spots[i].s.call);
    redraw();
}

static void js8_map_press(void) {
    int sel = find_sel();
    if (!m.canvas || !m.pick || sel < 0) return;
    js8_map_station_t st = m.spots[sel].s;   /* the pick may close the map */
    m.pick(&st);
}

static void key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {   /* MFK press */
        js8_map_press();
        return;
    }
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) js8_map_step(1);
    else if (key == LV_KEY_LEFT || key == LV_KEY_UP) js8_map_step(-1);
    else if (key == '+' || key == '-') js8_map_zoom(key == '+' ? 1 : -1);
}

static void delete_cb(lv_event_t *e) {
    free(lv_event_get_user_data(e));
    if (m.canvas != lv_event_get_target(e)) return;   /* a map closed after the next one opened */
    if (m.timer) lv_timer_del(m.timer);
    m.timer  = NULL;
    m.canvas = NULL;
}

lv_obj_t *js8_map_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, const char *my_grid, const char *log_path,
                         js8_map_fill_t fill, bool log_spots, js8_map_pick_t pick) {
    size_t   size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(w, h);
    uint8_t *buf  = malloc(2 * size);   /* the canvas, then the base */
    if (!buf) return NULL;
    if (m.timer) lv_timer_del(m.timer);   /* the map before, still being deleted */
    m.timer = NULL;

    char g4[5] = "";
    if (my_grid) snprintf(g4, sizeof(g4), "%s", my_grid);   /* the square only, never the full locator */
    m.my_lat = m.my_lon = 0.0;
    if (strlen(g4) >= 4) qth_str_to_pos(g4, &m.my_lat, &m.my_lon);
    m.w         = w;
    m.h         = h;
    m.fill      = fill;
    m.pick      = pick;
    m.log_spots = log_spots;
    m.zoom      = 0;
    m.sel[0]    = 0;
    m.base      = (lv_color_t *)(buf + size);
    memset(&m.base_view, 0, sizeof(m.base_view));
    m.log_n = log_path ? read_qsos(log_path, m.log, MAX_QSOS) : 0;

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_event_cb(canvas, delete_cb, LV_EVENT_DELETE, buf);
    lv_obj_add_event_cb(canvas, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(canvas, key_cb, LV_EVENT_CLICKED, NULL);
    m.canvas = canvas;
    redraw();
    if (fill) m.timer = lv_timer_create(timer_cb, REFRESH_MS, NULL);
    return canvas;
}

/* ---- Stations heard (FT8/FT4) ------------------------------------------- */

typedef struct {
    char    call[12];
    char    grid[5];
    int16_t snr;
    bool    heard_me;
    time_t  t;
    uint8_t data[sizeof(((js8_map_station_t *)0)->data)];
} heard_t;

static heard_t         heard[HEARD_MAX];
static int             heard_n;
static pthread_mutex_t heard_mutex = PTHREAD_MUTEX_INITIALIZER;

void js8_map_heard(const char *call, const char *grid, int snr, bool to_me, const void *data, size_t size) {
    if (!call || !call[0] || strpbrk(call, "<.") || strlen(call) >= sizeof(heard[0].call))
        return;   /* not an unresolved hash */
    char g[5] = "";
    if (grid && strlen(grid) >= 4) {
        memcpy(g, grid, 4);
        if (!qth_grid_check(g) || !strcmp(g, "RR73")) g[0] = 0;
    }

    pthread_mutex_lock(&heard_mutex);
    int i = 0, old = 0;
    for (; i < heard_n && strcmp(heard[i].call, call); i++)
        if (heard[i].t < heard[old].t) old = i;
    if (i == heard_n) {
        if (heard_n < HEARD_MAX) heard_n++;
        else i = old;   /* full: the oldest goes */
        memset(&heard[i], 0, sizeof(heard[i]));
    }
    snprintf(heard[i].call, sizeof(heard[i].call), "%s", call);
    if (g[0]) memcpy(heard[i].grid, g, sizeof(g));   /* else the one it sent before, if any */
    heard[i].snr = (int16_t)snr;
    heard[i].heard_me |= to_me;
    heard[i].t = time(NULL);
    if (data) memcpy(heard[i].data, data, size < sizeof(heard[i].data) ? size : sizeof(heard[i].data));
    pthread_mutex_unlock(&heard_mutex);
}

void js8_map_heard_clear(void) {
    pthread_mutex_lock(&heard_mutex);
    heard_n = 0;
    pthread_mutex_unlock(&heard_mutex);
}

int js8_map_heard_fill(js8_map_station_t *out, int max) {
    time_t now = time(NULL);
    int    n   = 0;
    pthread_mutex_lock(&heard_mutex);
    for (int i = 0; i < heard_n && n < max; i++) {
        if (now - heard[i].t > HEARD_S) continue;
        js8_map_station_t *s = &out[n++];
        snprintf(s->call, sizeof(s->call), "%s", heard[i].call);
        snprintf(s->grid, sizeof(s->grid), "%s", heard[i].grid);
        s->snr      = heard[i].snr;
        s->t        = heard[i].t;
        s->heard_me = heard[i].heard_me;
        memcpy(s->data, heard[i].data, sizeof(s->data));
    }
    pthread_mutex_unlock(&heard_mutex);
    return n;
}
