/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The Ops Room theme's own pieces (ops_room.h). Drawn by the radio, the words its own.
 */
#include "ops_room.h"

#include "app_window.h"
#include "board.h"
#include "gui_styles.h"
#include "gui_events.h"
#include "smeter.h"
#include "theme.h"
#include "tx_meters.h"

#include <stdio.h>

/* ---- Text ---- */

static void text(lv_draw_ctx_t *dc, const lv_font_t *font, lv_color_t col, lv_coord_t x, lv_coord_t y,
                 lv_text_align_t align, const char *s) {
    lv_draw_label_dsc_t d;
    lv_point_t          sz;

    lv_draw_label_dsc_init(&d);
    d.font  = font;
    d.color = col;
    lv_txt_get_size(&sz, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (align == LV_TEXT_ALIGN_RIGHT) x -= sz.x;
    else if (align == LV_TEXT_ALIGN_CENTER) x -= sz.x / 2;
    lv_area_t a = {x, y, x + sz.x, y + sz.y};
    lv_draw_label(dc, &d, &a, s, NULL);
}

static void rect(lv_draw_ctx_t *dc, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, lv_color_t col) {
    lv_draw_rect_dsc_t d;
    lv_area_t          a = {x1, y1, x2, y2};

    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    lv_draw_rect(dc, &d, &a);
}

/* ---- 7-segment digits ---- */

/* Segments a..g as bits 0..6 */
static const uint8_t seg_of[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};

/* A digit w x h at x, y, segments t thick; the unlit ones faintly there, as on a real display */
static void digit(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_coord_t t, int n,
                  lv_color_t on, lv_color_t off) {
    uint8_t    s   = n >= 0 && n <= 9 ? seg_of[n] : 0x40;
    lv_coord_t mid = y + (h - t) / 2, x2 = x + w - 1, y2 = y + h - 1;

#define SEG(bit, X1, Y1, X2, Y2) rect(dc, X1, Y1, X2, Y2, (s >> bit) & 1 ? on : off)
    SEG(0, x + t, y, x2 - t, y + t - 1);                    /* a */
    SEG(1, x2 - t + 1, y + t, x2, mid - 1);                 /* b */
    SEG(2, x2 - t + 1, mid + t, x2, y2 - t);                /* c */
    SEG(3, x + t, y2 - t + 1, x2 - t, y2);                  /* d */
    SEG(4, x, mid + t, x + t - 1, y2 - t);                  /* e */
    SEG(5, x, y + t, x + t - 1, mid - 1);                   /* f */
    SEG(6, x + t, mid, x2 - t, mid + t - 1);                /* g */
#undef SEG
}

static void colon(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y, lv_coord_t h, lv_coord_t t, lv_color_t col) {
    rect(dc, x, y + h / 3 - t / 2, x + t - 1, y + h / 3 + t / 2, col);
    rect(dc, x, y + 2 * h / 3 - t / 2, x + t - 1, y + 2 * h / 3 + t / 2, col);
}

/* ---- The activity timer ---- */

void ops_room_clock_draw(lv_obj_t *o, lv_draw_ctx_t *dc, const ops_room_clock_t *c) {
    lv_coord_t x1 = o->coords.x1, x2 = o->coords.x2, y1 = o->coords.y1;
    lv_color_t on = TC(text_head), off = TC(off), dim = TC(text_dim);
    char       buf[16];
    bool       low = c->known && c->low;

    /* INTERNAL (red on battery) or EXTERNAL, and the charge; nothing until the first reading of the power, and
     * NO BATT on a supply without one */
    if (c->known) {
        text(dc, &sony_14, c->ext ? TC(ok) : TC(err), x1 + 12, y1 + 4, LV_TEXT_ALIGN_LEFT,
             c->ext ? "EXTERNAL" : "INTERNAL");
        if (c->battery) snprintf(buf, sizeof(buf), "%u%%", c->cap);
        else snprintf(buf, sizeof(buf), "NO BATT");
        text(dc, &sony_14, low ? TC(err) : c->battery ? TC(text) : dim, x2 - 12, y1 + 4, LV_TEXT_ALIGN_RIGHT, buf);
    }

    /* HH:MM big, :SS small, the lot centred */
    const lv_coord_t W = 16, H = 26, T = 3, G = 3, w = 9, h = 15, t = 2;
    lv_coord_t       total = 4 * W + 3 * G + T + 2 * G + 6 + 2 * w + 2, x = (x1 + x2 + 1) / 2 - total / 2, y = y1 + 17;

    digit(dc, x, y, W, H, T, c->h / 10, on, off);
    x += W + G;
    digit(dc, x, y, W, H, T, c->h % 10, on, off);
    x += W + G;
    colon(dc, x, y, H, T, on);
    x += T + G;
    digit(dc, x, y, W, H, T, c->m / 10, on, off);
    x += W + G;
    digit(dc, x, y, W, H, T, c->m % 10, on, off);
    x += W + 6;
    digit(dc, x, y + H - h, w, h, t, c->s / 10, on, off);
    x += w + 2;
    digit(dc, x, y + H - h, w, h, t, c->s % 10, on, off);

    /* The limit: the battery's time left (on external power, to full), written as the other themes write it,
     * "~3h56"; without a battery the supply's voltage */
    if (c->known && !c->battery) {
        text(dc, &sony_14, dim, x1 + 12, y1 + 46, LV_TEXT_ALIGN_LEFT, "SUPPLY");
        snprintf(buf, sizeof(buf), "%.1f V", c->v_ext);
    } else {
        text(dc, &sony_14, dim, x1 + 12, y1 + 46, LV_TEXT_ALIGN_LEFT, c->known && c->ext ? "TO FULL" : "LIMIT");
        if (c->known && c->est_min >= 0)
            snprintf(buf, sizeof(buf), "%s%dh%02d", c->approx ? "~" : "", c->est_min / 60, c->est_min % 60);
        else if (c->known && c->full) snprintf(buf, sizeof(buf), "FULL");
        else snprintf(buf, sizeof(buf), "--h--");
    }
    text(dc, &sony_14, low ? TC(err) : TC(text), x2 - 12, y1 + 46, LV_TEXT_ALIGN_RIGHT, buf);
}

/* ---- The S-meter in hexagons ---- */

/* A flat-topped hexagon w x h at x, y */
static void hexagon(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t col) {
    lv_draw_rect_dsc_t d;
    lv_coord_t         q = w / 4;
    lv_point_t         p[6] = {{x + q, y},         {x + w - 1 - q, y},     {x + w - 1, y + h / 2},
                               {x + w - 1 - q, y + h - 1}, {x + q, y + h - 1}, {x, y + h / 2}};

    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    lv_draw_polygon(dc, &d, p, 6);
}

void ops_room_meter_draw(lv_obj_t *o, lv_draw_ctx_t *dc, float db, float peak, float value, bool tx) {
    static const struct { int16_t db; const char *t; } marks[] = {
        {S1, "1"}, {S3, "3"}, {S1 + 24, "5"}, {S1 + 36, "7"}, {S9, "9"}, {S9_20, "+20"}, {S9_40, "+40"}};
    lv_coord_t x0 = o->coords.x1 + 12, x1 = o->coords.x2 - 12, w = x1 - x0, y0 = o->coords.y1;
    char       buf[16];

    text(dc, &sony_20, TC(text_head), x0, y0 + 4, LV_TEXT_ALIGN_LEFT, "SIGNAL");
    snprintf(buf, sizeof(buf), "%.1f", value);
    text(dc, &sony_20, tx ? TC(err) : TC(text_value), x1, y0 + 4, LV_TEXT_ALIGN_RIGHT, tx ? "TX" : buf);

    /* 18 cells 14 px wide, overlapping into a honeycomb row; S9 at 70%, as Codec's SIGNAL bar */
    const int  n = 18;
    lv_coord_t cw = 14, step = (w - cw) / (n - 1), lvl = meter_x70(db, w), pk = meter_x70(peak, w);
    lv_coord_t s9 = meter_x70(S9, w);

    for (int i = 0; i < n; i++) {
        lv_coord_t cx = i * step, mid = cx + cw / 2;
        bool       lit = mid < lvl, top = !lit && mid <= pk && mid + step > pk;
        lv_color_t col = top ? TC(text_value) : !lit ? TC(meter_weak) : mid >= s9 ? TC(meter_over) : TC(meter_bar);

        hexagon(dc, x0 + cx, y0 + (i % 2 ? 29 : 26), cw, 12, col);
    }

    for (int i = 0; i < (int)(sizeof(marks) / sizeof(marks[0])); i++)
        text(dc, &sony_14, marks[i].db > S9 ? TC(meter_over) : TC(text_dim), x0 + meter_x70(marks[i].db, w), y0 + 45,
             LV_TEXT_ALIGN_CENTER, marks[i].t);
}

/* ---- The TRANSMIT band while the radio transmits ---- */

#define BAND_H  40
#define PLATE_W 300

static lv_obj_t *screen;    /* the main screen: the band is one of its objects */
static lv_obj_t *band;

static void band_draw(lv_event_t *e) {
    lv_draw_ctx_t     *dc = lv_event_get_draw_ctx(e);
    lv_obj_t          *o  = lv_event_get_target(e);
    lv_coord_t         x1 = o->coords.x1, y1 = o->coords.y1, y2 = o->coords.y2;
    lv_draw_rect_dsc_t d;

    rect(dc, x1, y1, o->coords.x2, y2, lv_color_black());
    lv_draw_rect_dsc_init(&d);
    d.bg_color = TC(tx);
    for (lv_coord_t x = x1 - BAND_H; x < o->coords.x2; x += 24) {   /* stripes leaning right, 12 px wide */
        lv_point_t p[4] = {{x, y2}, {x + BAND_H, y1}, {x + BAND_H + 12, y1}, {x + 12, y2}};
        lv_draw_polygon(dc, &d, p, 4);
    }
    lv_coord_t px = (x1 + o->coords.x2) / 2 - PLATE_W / 2;
    rect(dc, px - 3, y1, px + PLATE_W + 2, y2, TC(tx));
    rect(dc, px, y1 + 3, px + PLATE_W - 1, y2 - 3, lv_color_black());
    text(dc, &sony_14, TC(tx), px + 10, y1 + 6, LV_TEXT_ALIGN_LEFT, "ON AIR");
    text(dc, &sony_14, TC(tx), px + PLATE_W - 10, y1 + 6, LV_TEXT_ALIGN_RIGHT, "TX");
    text(dc, &sony_32, TC(tx), px + PLATE_W / 2, y1 + 8, LV_TEXT_ALIGN_CENTER, "TRANSMIT");
}

static void band_deleted(lv_event_t *e) {
    band = NULL;    /* with the main screen's widgets on a theme change (main_screen_set_theme cleans the screen) */
}

/* On the main screen only, under what opens over it: not on lv_layer_sys(), where it covered the apps while they
 * transmit (an FT8 cycle's decode list), the walkie's bandscope and F3 key at PTT, and the foot of a message shown
 * in TX. An app has the TX shown its own way: no band then; one opened in TX is over it. The walkie is a screen of
 * its own */
static void radio_cb(lv_event_t *e) {
    bool tx = lv_event_get_code(e) == EVENT_RADIO_TX;

    if (!theme_shape.ops_room || !tx || dialog_is_run()) {
        if (band) lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!band) {
        band = lv_obj_create(screen);
        lv_obj_remove_style_all(band);
        lv_obj_set_size(band, BOARD_DISP_W, BAND_H);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(band, band_draw, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(band, band_deleted, LV_EVENT_DELETE, NULL);
    }
    lv_obj_set_pos(band, 0, tx_info_bottom() + 8);     /* under the TX panel, over the waterfall */
    lv_obj_clear_flag(band, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(band);
}

void ops_room_init(lv_obj_t *main_screen) {
    screen = main_screen;
    lv_obj_add_event_cb(screen, radio_cb, EVENT_RADIO_TX, NULL);
    lv_obj_add_event_cb(screen, radio_cb, EVENT_RADIO_RX, NULL);
}
