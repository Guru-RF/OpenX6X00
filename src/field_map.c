/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops map
 *
 *  Tiles: web mercator, 256 px JPEG, BOARD_DATA/field/tiles/<z>/<x>/<y>.jpg, made on a PC from aerial photographs or
 *  a map whose licence allows it, at three zooms: 13 (about 3 km a tile, the whole area), 15 (800 m, around the
 *  references) and 17 (200 m, close around them). tiles/credit.txt, one line, is the credit the source asks for,
 *  drawn in the corner. No network: what is not on the card is drawn as an empty dark square, a view with no tile at
 *  all says there is no map here. Without the tiles directory there is no map (field_map_available).
 *
 *  The automatic zoom is the closest of the three where the chosen reference and the radio's position both fit, the
 *  view centred between them; without a position, around the reference at 17. Further than zoom 13 holds, the view
 *  stays on the reference at 13 and an arrow on the edge points to the radio, with the distance. The zoom set by hand
 *  holds FIELD_MAP_MANUAL_S, then the automatic one comes back.
 *
 *  Drawn only when something changes (the reference, the zoom, the position by a pixel or more, the references
 *  nearby): the decoded tiles stay in a small cache (CACHE_N), so a redraw decodes only what came into view. The
 *  photographs are darkened a little, for the night themes.
 */

#include "field_map.h"

#include "gui_styles.h"
#include "theme.h"
#include "board.h"

#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include <jpeglib.h>

#define TILES_DIR   BOARD_DATA "/field/tiles"
#define CREDIT_PATH TILES_DIR "/credit.txt"
#define TILE        256
#define CACHE_N     16
#define MARGIN      28          /* the position and the reference stay that far inside the edges */
#define DARKEN      LV_OPA_40   /* the photographs under the markers */
#define ME_COLOR    0x78beff    /* the radio's position, apart from the theme's reds and greens */

static const int levels[] = {17, 15, 13};
#define NLEVELS (int)(sizeof(levels) / sizeof(levels[0]))

typedef struct {
    int          z, x, y;
    bool         missing;       /* not on the card: an empty square, not looked for again */
    uint32_t     used;
    lv_img_dsc_t dsc;
} tile_t;

static tile_t   cache[CACHE_N];
static uint32_t use_clock;

static lv_obj_t *map;
static bool      have_target, have_me;
static double    t_lat, t_lon, m_lat, m_lon;
static int       m_acc;
static char      t_name[48];
static char      ov1[64], ov2[64];
static field_map_pt_t pts[40];
static int       npts;
static int       manual_level = -1;
static time_t    manual_until;
static char      credit[48];    /* CREDIT_PATH's line, read as a map is made */

/* World pixels at zoom z */
static double wx(double lon, int z) {
    return (lon + 180.0) / 360.0 * TILE * (double)(1 << z);
}

static double wy(double lat, int z) {
    double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * TILE * (double)(1 << z);
}

static double metres(double la1, double lo1, double la2, double lo2) {
    double r = M_PI / 180.0, dl = (lo2 - lo1) * r;
    double a = sin((la2 - la1) * r / 2) * sin((la2 - la1) * r / 2) + cos(la1 * r) * cos(la2 * r) * sin(dl / 2) * sin(dl / 2);
    return 6371000.0 * 2 * asin(sqrt(a));
}

/* libjpeg's errors come back here instead of ending the program */
struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf               back;
};

static void jerr_exit(j_common_ptr c) {
    longjmp(((struct jerr *)c->err)->back, 1);
}

/* The tile as lv_color32_t (B, G, R, A), NULL when the file is not there or not a 256 px JPEG */
static uint8_t *decode(const char *path) {
    FILE                         *f = fopen(path, "rb");
    struct jpeg_decompress_struct c;
    struct jerr                   e;
    uint8_t                      *out = NULL, *row = NULL;

    if (!f) return NULL;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = jerr_exit;
    if (setjmp(e.back)) {
        jpeg_destroy_decompress(&c);
        fclose(f);
        free(out);
        free(row);
        return NULL;
    }
    jpeg_create_decompress(&c);
    jpeg_stdio_src(&c, f);
    jpeg_read_header(&c, TRUE);
    c.out_color_space = JCS_RGB;
    jpeg_start_decompress(&c);
    if (c.output_width != TILE || c.output_height != TILE || c.output_components != 3) {
        jpeg_destroy_decompress(&c);
        fclose(f);
        return NULL;
    }
    out = malloc(TILE * TILE * 4);
    row = malloc(TILE * 3);
    if (!out || !row) longjmp(e.back, 1);
    while (c.output_scanline < c.output_height) {
        uint8_t *d = out + c.output_scanline * TILE * 4;

        jpeg_read_scanlines(&c, &row, 1);
        for (int i = 0; i < TILE; i++) {
            d[i * 4 + 0] = row[i * 3 + 2];
            d[i * 4 + 1] = row[i * 3 + 1];
            d[i * 4 + 2] = row[i * 3 + 0];
            d[i * 4 + 3] = 0xff;
        }
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    fclose(f);
    free(row);
    return out;
}

/* The tile from the cache, decoded on its first use (the least used one makes room); NULL: not on the card */
static const lv_img_dsc_t *tile(int z, int x, int y) {
    tile_t *t = NULL;
    char    path[256];  /* the PC build's data folder can be a long path */

    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].z == z && cache[i].x == x && cache[i].y == y && (cache[i].dsc.data || cache[i].missing)) {
            cache[i].used = ++use_clock;
            return cache[i].missing ? NULL : &cache[i].dsc;
        }
    for (int i = 0; i < CACHE_N; i++)
        if (!t || cache[i].used < t->used) t = &cache[i];
    free((void *)t->dsc.data);
    memset(t, 0, sizeof(*t));
    t->z = z;
    t->x = x;
    t->y = y;
    t->used = ++use_clock;
    snprintf(path, sizeof(path), TILES_DIR "/%d/%d/%d.jpg", z, x, y);
    uint8_t *data = decode(path);
    if (!data) {
        t->missing = true;
        return NULL;
    }
    t->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    t->dsc.header.w = TILE;
    t->dsc.header.h = TILE;
    t->dsc.data_size = TILE * TILE * 4;
    t->dsc.data = data;
    return &t->dsc;
}

/* The card has the tile under that point at zoom z (closer zooms cover only around the references: a view where the
 * radio stands outside them would be half empty) */
static bool covered(int z, double lat, double lon) {
    return tile(z, (int)(wx(lon, z) / TILE), (int)(wy(lat, z) / TILE)) != NULL;
}

/* The view: its zoom and its centre in world pixels; me_in: the position is inside it */
static void view(lv_coord_t w, lv_coord_t h, int *z, double *cx, double *cy, bool *me_in) {
    int lv = -1;

    *me_in = false;
    if (manual_level >= 0 && time(NULL) < manual_until) {
        lv = manual_level;
    } else {
        manual_level = -1;
        if (have_target && have_me) {
            for (int i = 0; i < NLEVELS && lv < 0; i++) {
                int z1 = levels[i];
                if (fabs(wx(t_lon, z1) - wx(m_lon, z1)) < w - 2 * MARGIN && fabs(wy(t_lat, z1) - wy(m_lat, z1)) < h - 2 * MARGIN
                    && covered(z1, t_lat, t_lon) && covered(z1, m_lat, m_lon))
                    lv = i;
            }
        }
        if (lv < 0 && have_target && !have_me)     /* around the reference alone: the closest the card has */
            for (int i = 0; i < NLEVELS && lv < 0; i++)
                if (covered(levels[i], t_lat, t_lon)) lv = i;
        if (lv < 0) lv = NLEVELS - 1;
    }
    *z = levels[lv];
    if (have_target && have_me && manual_level < 0 &&
        fabs(wx(t_lon, *z) - wx(m_lon, *z)) < w - 2 * MARGIN && fabs(wy(t_lat, *z) - wy(m_lat, *z)) < h - 2 * MARGIN) {
        *cx = (wx(t_lon, *z) + wx(m_lon, *z)) / 2;
        *cy = (wy(t_lat, *z) + wy(m_lat, *z)) / 2;
    } else if (have_target) {
        *cx = wx(t_lon, *z);
        *cy = wy(t_lat, *z);
    } else if (have_me) {
        *cx = wx(m_lon, *z);
        *cy = wy(m_lat, *z);
    } else {                /* nothing to put on it: anywhere, the map says there is none here */
        *cx = wx(0, *z);
        *cy = wy(0, *z);
    }
    if (have_me) {
        double dx = wx(m_lon, *z) - *cx, dy = wy(m_lat, *z) - *cy;
        *me_in = fabs(dx) < w / 2 - 8 && fabs(dy) < h / 2 - 8;
    }
}

static void text_box(lv_draw_ctx_t *ctx, lv_coord_t x, lv_coord_t y, const char *s, const lv_font_t *font, lv_color_t col) {
    lv_draw_label_dsc_t t;
    lv_draw_rect_dsc_t  r;
    lv_point_t          size;
    lv_area_t           a;

    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, 0);
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_black();
    r.bg_opa = LV_OPA_70;
    a.x1 = x - 4;
    a.y1 = y - 2;
    a.x2 = x + size.x + 4;
    a.y2 = y + size.y + 2;
    lv_draw_rect(ctx, &r, &a);
    lv_draw_label_dsc_init(&t);
    t.font = font;
    t.color = col;
    a.x1 = x;
    a.y1 = y;
    a.x2 = x + size.x;
    a.y2 = y + size.y;
    lv_draw_label(ctx, &t, &a, s, NULL);
}

static void line(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, lv_color_t c, int w) {
    lv_draw_line_dsc_t l;
    lv_point_t         a = {x1, y1}, b = {x2, y2};

    lv_draw_line_dsc_init(&l);
    l.color = c;
    l.width = w;
    l.round_start = l.round_end = 1;
    lv_draw_line(ctx, &l, &a, &b);
}

/* A reference nearby, by its kind: square bunker, triangle memorial, diamond remains, ring abandoned, dot else */
static void marker(lv_draw_ctx_t *ctx, lv_coord_t x, lv_coord_t y, char kind, bool done) {
    lv_draw_rect_dsc_t r;
    lv_area_t          a = {x - 6, y - 6, x + 6, y + 6};
    lv_color_t         c = done ? TC(text_mute) : TC(text);

    lv_draw_rect_dsc_init(&r);
    r.bg_color = c;
    r.border_color = lv_color_black();
    r.border_width = 1;
    switch (kind) {
        case 'B':
            lv_draw_rect(ctx, &r, &a);
            break;
        case 'M':
            for (int i = 0; i < 7; i++) line(ctx, x - i, y - 5 + i * 2, x + i, y - 5 + i * 2, c, 2);
            break;
        case 'V':
            for (int i = -6; i <= 6; i++) line(ctx, x - (6 - abs(i)), y + i, x + (6 - abs(i)), y + i, c, 1);
            break;
        case 'A':
            r.bg_opa = LV_OPA_TRANSP;
            r.border_color = c;
            r.border_width = 3;
            r.radius = LV_RADIUS_CIRCLE;
            lv_draw_rect(ctx, &r, &a);
            break;
        default:
            r.radius = LV_RADIUS_CIRCLE;
            lv_draw_rect(ctx, &r, &a);
    }
    if (done) {     /* ticked */
        line(ctx, x + 8, y - 2, x + 11, y + 2, TC(ok), 3);
        line(ctx, x + 11, y + 2, x + 17, y - 8, TC(ok), 3);
    }
}

static void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t      area;
    lv_coord_t     w, h;
    int            z;
    double         cx, cy;
    bool           me_in;
    char           s[48];

    lv_obj_get_coords(map, &area);
    w = lv_area_get_width(&area);
    h = lv_area_get_height(&area);
    view(w, h, &z, &cx, &cy, &me_in);

    /* the tiles, darkened; an empty dark square where the card has none */
    double ox = cx - w / 2.0, oy = cy - h / 2.0;
    int    tx0 = (int)floor(ox / TILE), ty0 = (int)floor(oy / TILE);
    int    tx1 = (int)floor((ox + w) / TILE), ty1 = (int)floor((oy + h) / TILE);
    lv_draw_img_dsc_t  img;
    lv_draw_rect_dsc_t empty;
    int                shown = 0;

    lv_draw_img_dsc_init(&img);
    img.recolor = lv_color_black();
    img.recolor_opa = DARKEN;
    lv_draw_rect_dsc_init(&empty);
    empty.bg_color = TC(panel);
    empty.border_color = TC(panel_border);
    empty.border_width = 1;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            lv_area_t a;

            a.x1 = area.x1 + (lv_coord_t)lround(tx * TILE - ox);
            a.y1 = area.y1 + (lv_coord_t)lround(ty * TILE - oy);
            a.x2 = a.x1 + TILE - 1;
            a.y2 = a.y1 + TILE - 1;
            const lv_img_dsc_t *d = tile(z, tx, ty);
            if (d) {
                lv_draw_img(ctx, &img, &a, d);
                shown++;
            } else
                lv_draw_rect(ctx, &empty, &a);
        }

#define SX(lon) (area.x1 + (lv_coord_t)lround(wx(lon, z) - ox))
#define SY(lat) (area.y1 + (lv_coord_t)lround(wy(lat, z) - oy))

    /* the references nearby, the chosen one apart */
    for (int i = 0; i < npts; i++) {
        lv_coord_t x = SX(pts[i].lon), y = SY(pts[i].lat);

        if (have_target && fabs(pts[i].lat - t_lat) < 1e-6 && fabs(pts[i].lon - t_lon) < 1e-6) continue;
        if (x < area.x1 + 6 || x > area.x2 - 6 || y < area.y1 + 6 || y > area.y2 - 6) continue;
        marker(ctx, x, y, pts[i].kind, pts[i].done);
    }

    lv_coord_t tx = have_target ? SX(t_lon) : 0, ty = have_target ? SY(t_lat) : 0;
    lv_color_t me_c = lv_color_hex(ME_COLOR);

    /* the radio: a dot, its accuracy as a ring when the ring shows; outside, an arrow on the edge */
    if (have_me && me_in) {
        lv_coord_t         mx = SX(m_lon), my = SY(m_lat);
        double             mpp = 156543.03 * cos(m_lat * M_PI / 180) / (1 << z);
        lv_coord_t         ring = (lv_coord_t)(m_acc / mpp);
        lv_draw_rect_dsc_t r;
        lv_area_t          a;

        if (have_target) line(ctx, mx, my, tx, ty, TC(text_soft), 1);
        lv_draw_rect_dsc_init(&r);
        r.radius = LV_RADIUS_CIRCLE;
        if (ring > 12) {
            r.bg_opa = LV_OPA_TRANSP;
            r.border_color = me_c;
            r.border_opa = LV_OPA_50;
            r.border_width = 1;
            a = (lv_area_t){mx - ring, my - ring, mx + ring, my + ring};
            lv_draw_rect(ctx, &r, &a);
        }
        r.bg_opa = LV_OPA_COVER;
        r.bg_color = me_c;
        r.border_color = lv_color_black();
        r.border_opa = LV_OPA_COVER;
        r.border_width = 2;
        a = (lv_area_t){mx - 8, my - 8, mx + 8, my + 8};
        lv_draw_rect(ctx, &r, &a);
    } else if (have_me && have_target) {
        double ang = atan2(wy(m_lat, z) - cy, wx(m_lon, z) - cx);
        double k = fmin((w / 2.0 - 18) / fmax(fabs(cos(ang)), 1e-6), (h / 2.0 - 18) / fmax(fabs(sin(ang)), 1e-6));
        lv_coord_t ax = area.x1 + (lv_coord_t)(w / 2.0 + k * cos(ang)), ay = area.y1 + (lv_coord_t)(h / 2.0 + k * sin(ang));

        lv_point_t tip = {ax + (lv_coord_t)(13 * cos(ang)), ay + (lv_coord_t)(13 * sin(ang))};
        lv_point_t l = {ax + (lv_coord_t)(11 * cos(ang + 2.4)), ay + (lv_coord_t)(11 * sin(ang + 2.4))};
        lv_point_t r = {ax + (lv_coord_t)(11 * cos(ang - 2.4)), ay + (lv_coord_t)(11 * sin(ang - 2.4))};
        for (int i = 0; i <= 10; i++) {     /* a filled triangle, line by line */
            lv_coord_t px = l.x + (r.x - l.x) * i / 10, py = l.y + (r.y - l.y) * i / 10;
            line(ctx, px, py, tip.x, tip.y, me_c, 2);
        }
        snprintf(s, sizeof(s), "you %.0f km", metres(m_lat, m_lon, t_lat, t_lon) / 1000);
        text_box(ctx, LV_CLAMP(area.x1 + 6, ax - 40 - (lv_coord_t)(30 * cos(ang)), area.x2 - 90),
                 LV_CLAMP(area.y1 + 6, ay - 10 - (lv_coord_t)(24 * sin(ang)), area.y2 - 26), s, &sony_16, me_c);
    }

    /* the chosen reference: a crosshair and its name */
    if (have_target) {
        lv_color_t         c = TC(vfo_line);
        lv_draw_rect_dsc_t r;
        lv_area_t          a = {tx - 12, ty - 12, tx + 12, ty + 12};

        lv_draw_rect_dsc_init(&r);
        r.bg_opa = LV_OPA_TRANSP;
        r.border_color = c;
        r.border_width = 3;
        r.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(ctx, &r, &a);
        line(ctx, tx - 20, ty, tx - 7, ty, c, 3);
        line(ctx, tx + 7, ty, tx + 20, ty, c, 3);
        line(ctx, tx, ty - 20, tx, ty - 7, c, 3);
        line(ctx, tx, ty + 7, tx, ty + 20, c, 3);
        if (t_name[0]) {    /* right of the crosshair, or left of it when it would run off the map */
            lv_point_t sz;
            char       nm[48];

            snprintf(nm, sizeof(nm), "%s", t_name);
            lv_txt_get_size(&sz, nm, &sony_18, 0, 0, LV_COORD_MAX, 0);
            while (sz.x > lv_area_get_width(&area) / 2 && strlen(nm) > 4) {    /* long names cut short */
                nm[strlen(nm) - 4] = 0;
                strcat(nm, "...");
                lv_txt_get_size(&sz, nm, &sony_18, 0, 0, LV_COORD_MAX, 0);
            }
            lv_coord_t nx = tx + 18 + sz.x + 8 < area.x2 ? tx + 18 : tx - 18 - sz.x;
            text_box(ctx, LV_MAX(nx, area.x1 + 6), LV_MAX(ty - 36, area.y1 + 4), nm, &sony_18, TC(text_value));
        }
    }

    /* scale, north, zoom */
    double     mpp = 156543.03 * cos((have_target ? t_lat : m_lat) * M_PI / 180) / (1 << z);
    int        bar_m = z >= 17 ? 50 : z >= 15 ? 200 : 1000;
    lv_coord_t bar = (lv_coord_t)(bar_m / mpp);

    line(ctx, area.x1 + 10, area.y2 - 12, area.x1 + 10 + bar, area.y2 - 12, TC(text_value), 3);
    snprintf(s, sizeof(s), bar_m >= 1000 ? "%d km" : "%d m", bar_m >= 1000 ? bar_m / 1000 : bar_m);
    text_box(ctx, area.x1 + 10, area.y2 - 34, s, &sony_14, TC(text_value));
    text_box(ctx, area.x2 - 20, area.y1 + 6, "N", &sony_16, TC(vfo_line));
    /* the zoom, automatic or by hand, and the tiles' credit, right-aligned in the bottom corner */
    lv_point_t sz;

    snprintf(s, sizeof(s), "%s%s%s", manual_level >= 0 ? "ZOOM" : "AUTO", credit[0] ? "  -  " : "", credit);
    lv_txt_get_size(&sz, s, &sony_14, 0, 0, LV_COORD_MAX, 0);
    text_box(ctx, area.x2 - 10 - sz.x, area.y2 - 24, s, &sony_14, TC(text_dim));
    if (ov1[0]) {
        text_box(ctx, area.x1 + 10, area.y1 + 8, ov1, &sony_20, TC(text_head));
        if (ov2[0]) text_box(ctx, area.x1 + 10, area.y1 + 36, ov2, &sony_18, TC(text_value));
    }

    /* not one tile in the view: the place is outside the area the card was made for, say so rather than show black */
    if (!shown) {
        static const char *l1 = "No map here", *l2 = "outside the area of the tiles on the card";
        lv_point_t         s1, s2;
        lv_coord_t         mx = area.x1 + w / 2, my = area.y1 + h / 2 + 34;

        lv_txt_get_size(&s1, l1, &sony_20, 0, 0, LV_COORD_MAX, 0);
        lv_txt_get_size(&s2, l2, &sony_16, 0, 0, LV_COORD_MAX, 0);
        text_box(ctx, mx - s1.x / 2, my, l1, &sony_20, TC(text_head));
        text_box(ctx, mx - s2.x / 2, my + 28, l2, &sony_16, TC(text_dim));
    }
#undef SX
#undef SY
}

bool field_map_available(void) {
    struct stat sb;

    return stat(TILES_DIR, &sb) == 0 && S_ISDIR(sb.st_mode);
}

/* The credit line, its newline and anything past 47 characters left out; "" without the file */
static void credit_load(void) {
    FILE *f = fopen(CREDIT_PATH, "r");

    credit[0] = 0;
    if (!f)
        return;
    if (!fgets(credit, sizeof(credit), f))
        credit[0] = 0;
    credit[strcspn(credit, "\r\n")] = 0;
    fclose(f);
}

lv_obj_t *field_map_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    credit_load();
    map = lv_obj_create(parent);
    lv_obj_remove_style_all(map);
    lv_obj_set_pos(map, x, y);
    lv_obj_set_size(map, w, h);
    lv_obj_clear_flag(map, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(map, TC(panel), 0);
    lv_obj_set_style_bg_opa(map, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(map, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    ov1[0] = ov2[0] = 0;
    return map;
}

void field_map_gone(void) {
    map = NULL;
}

static void redraw(void) {
    if (map) lv_obj_invalidate(map);
}

void field_map_target(bool have, double lat, double lon, const char *name) {
    if (have == have_target && (!have || (lat == t_lat && lon == t_lon)) && !strcmp(name ? name : "", t_name))
        return;
    have_target = have && isfinite(lat) && isfinite(lon);
    t_lat = lat;
    t_lon = lon;
    snprintf(t_name, sizeof(t_name), "%s", name ? name : "");
    manual_level = -1;      /* another reference: the automatic zoom */
    redraw();
}

void field_map_me(bool have, double lat, double lon, int acc_m) {
    have = have && isfinite(lat) && isfinite(lon);
    /* a move smaller than a pixel at zoom 17 (about 0.8 m) changes nothing on the screen */
    if (have == have_me && (!have || (metres(lat, lon, m_lat, m_lon) < 0.8 && abs(acc_m - m_acc) < 5)))
        return;
    have_me = have;
    m_lat = lat;
    m_lon = lon;
    m_acc = acc_m;
    redraw();
}

void field_map_points(const field_map_pt_t *p, int n) {
    n = LV_MIN(n, (int)(sizeof(pts) / sizeof(pts[0])));
    if (n == npts && !memcmp(p, pts, n * sizeof(*p)))
        return;
    memcpy(pts, p, n * sizeof(*p));
    npts = n;
    redraw();
}

void field_map_overlay(const char *line1, const char *line2) {
    if (!strcmp(line1, ov1) && !strcmp(line2, ov2))
        return;
    snprintf(ov1, sizeof(ov1), "%s", line1);
    snprintf(ov2, sizeof(ov2), "%s", line2);
    redraw();
}

void field_map_zoom(int d) {
    int cur;
    double cx, cy;
    bool in;
    lv_area_t a;

    if (!map) return;
    lv_obj_get_coords(map, &a);
    view(lv_area_get_width(&a), lv_area_get_height(&a), &cur, &cx, &cy, &in);
    int i = 0;
    while (i < NLEVELS - 1 && levels[i] != cur) i++;
    manual_level = LV_CLAMP(0, i - d, NLEVELS - 1);
    manual_until = time(NULL) + FIELD_MAP_MANUAL_S;
    redraw();
}

void field_map_tick(void) {
    if (manual_level >= 0 && time(NULL) >= manual_until) {
        manual_level = -1;
        redraw();
    }
}
