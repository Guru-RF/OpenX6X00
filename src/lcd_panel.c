/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  KX3Lovers Plus: the radio's state on amber glass, laid out as the LCD drawn in Elecraft's KX3
 *  owner's manual (Rev C5, page 8): the bar graph with its scales, CWT and the filter graphic on the
 *  left, VFO A in seven segments, the icons and VFO B in fourteen segments under it, the VFO and mode
 *  icons on the right; what is lit shows, the unlit digit segments as a faint outline. The manual's
 *  drawing (1248 x 313 units) is stretched a little to fill the top of the screen. Under it, what the
 *  KX3 has not: a spectrum strip (the spectrum itself, screen_main.c; in CW the decoded text beside
 *  it), power, IF, the time and the voltage.
 *
 *  Copyright (c) 2026 Pavol Calfa OM0WT
 */

#include "lcd_panel.h"

#include "cfg/cfg_api.h"
#include "clock.h"
#include "cw_tune_view.h"
#include "freq_scale.h"
#include "info_bar.h"
#include "gui_styles.h"
#include "radio.h"
#include "radio_types.h"
#include "screen_main.h"
#include "smeter.h"
#include "theme.h"
#include "tx_meters.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lv_obj_t      *panel;
static lv_obj_t      *strip;            /* the spectrum strip */
static lv_timer_t    *timer;

/* CW: the decoded text in the strip's right half while keying or while text comes in */
#define CW_HOLD_MS  10000
static pthread_mutex_t cw_mux = PTHREAD_MUTEX_INITIALIZER;
static char            cw_line[160];
static uint16_t        cw_seq;
static uint32_t        cw_ms;           /* lv_tick_get() of the last text */
static lv_coord_t     ox, oy;           /* the drawing's top left on the screen */
static lv_draw_ctx_t *ctx;

/* What is on the glass: drawn again only when it changes */
typedef struct {
    int32_t fa, fb, lo, hi;
    int8_t  mode, agc, bars, swr_bars, rf_bars;
    uint8_t flags;      /* tx, split, lock */
    uint16_t icons;
    int16_t sdb, swr10, pwr10, volts10, minute;
    uint16_t cw_seq;
    bool    cw;
    bool    cwt, cwt_on;    /* CW with the tuning aid on; a tone locked in the last second */
    int16_t cwt_hz;         /* its offset from the pitch */
    int32_t bw;
} lcd_state_t;

static lcd_state_t shown;

enum { I_VOX, I_ATU, I_RIT, I_XIT, I_SPLT, I_ATT, I_PRE, I_NB, I_NR, I_NTCH };

/* Units to pixels: the manual's drawing for the KX3 part, 1:1 for what is under it */
#define ART_W   1248
#define ART_H   313
#define KX_X    (800.0f / ART_W)
#define KX_Y    ((float)LCD_PLUS_LCD_H / ART_H)
static float kx = 1, ky = 1;

static lv_coord_t UX(float v) {
    return lroundf(v * kx);
}

static lv_coord_t UY(float v) {
    return lroundf(v * ky);
}

static lv_color_t ink(bool lit) {
    return lit ? TC(text) : TC(track);
}

/* Text with its left edge at x and its middle at y (units); returns the x after it */
static int text(int x, int y, const char *s, bool lit, const lv_font_t *font) {
    lv_draw_label_dsc_t dsc;
    lv_point_t          size;

    lv_draw_label_dsc_init(&dsc);
    dsc.font  = font;
    dsc.color = ink(lit);
    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (!lit) return x + lroundf(size.x / kx);
    lv_area_t a = {.x1 = ox + UX(x), .y1 = oy + UY(y) - size.y / 2};
    a.x2 = a.x1 + size.x;
    a.y2 = a.y1 + size.y;
    lv_draw_label(ctx, &dsc, &a, s, NULL);
    return x + lroundf(size.x / kx);
}

/* Centred on x */
static void text_c(int x, int y, const char *s, bool lit, const lv_font_t *font) {
    lv_point_t size;

    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    text(x - lroundf(size.x / (2 * kx)), y, s, lit, font);
}

/* Ending at x */
static int text_r(int x, int y, const char *s, bool lit, const lv_font_t *font) {
    lv_point_t size;

    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    x -= lroundf(size.x / kx);
    text(x, y, s, lit, font);
    return x;
}

static void rect(int x1, int y1, int x2, int y2, bool lit) {
    lv_draw_rect_dsc_t dsc;

    if (!lit) return;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = ink(lit);
    lv_area_t a = {ox + UX(x1), oy + UY(y1), ox + UX(x2), oy + UY(y2)};
    lv_draw_rect(ctx, &dsc, &a);
}

static void line(int x1, int y1, int x2, int y2, int w, bool lit) {
    lv_draw_line_dsc_t dsc;

    if (!lit) return;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = ink(lit);
    dsc.width = w;
    lv_point_t p1 = {ox + UX(x1), oy + UY(y1)}, p2 = {ox + UX(x2), oy + UY(y2)};
    lv_draw_line(ctx, &dsc, &p1, &p2);
}

static void poly(const lv_point_t *u, int n, bool lit) {
    lv_draw_rect_dsc_t dsc;
    lv_point_t         p[8];

    if (!lit) return;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = ink(lit);
    for (int i = 0; i < n; i++) p[i] = (lv_point_t){ox + UX(u[i].x), oy + UY(u[i].y)};
    lv_draw_polygon(ctx, &dsc, p, n);
}

/* A triangle pointing up (dir 0), down (1), left (2) or right (3), centred on x, y */
static void tri(int x, int y, int r, int dir, bool lit) {
    lv_point_t p[3];

    switch (dir) {
        case 0: p[0] = (lv_point_t){x - r, y + r}; p[1] = (lv_point_t){x + r, y + r}; p[2] = (lv_point_t){x, y - r}; break;
        case 1: p[0] = (lv_point_t){x - r, y - r}; p[1] = (lv_point_t){x + r, y - r}; p[2] = (lv_point_t){x, y + r}; break;
        case 2: p[0] = (lv_point_t){x + r, y - r}; p[1] = (lv_point_t){x + r, y + r}; p[2] = (lv_point_t){x - r, y}; break;
        default: p[0] = (lv_point_t){x - r, y - r}; p[1] = (lv_point_t){x - r, y + r}; p[2] = (lv_point_t){x + r, y}; break;
    }
    poly(p, 3, lit);
}

/* A frame around [x1, x2] x [y1, y2], the corners round by r (units) */
static void frame(int x1, int y1, int x2, int y2, int r, bool lit) {
    lv_draw_rect_dsc_t dsc;

    if (!lit) return;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa       = LV_OPA_TRANSP;
    dsc.border_color = ink(lit);
    dsc.border_width = 2;
    dsc.radius       = UX(r);
    lv_area_t a = {ox + UX(x1), oy + UY(y1), ox + UX(x2), oy + UY(y2)};
    lv_draw_rect(ctx, &dsc, &a);
}

static void boxed(int x1, int x2, int y, const char *s, bool lit) {
    frame(x1, y - 16, x2, y + 16, 8, lit);
    text_c((x1 + x2) / 2, y, s, lit, &sony_18);
}

static void lock_icon(int x, int y, bool lit) {
    frame(x - 9, y - 16, x + 9, y + 2, 9, lit);
    rect(x - 14, y - 4, x + 14, y + 16, lit);
}

/* How a display's places are drawn */
typedef struct {
    int  dw_pct;        /* the digit's width, % of its place */
    int  thick;         /* the stroke: the digit's width / thick */
    int  lean_pct;      /* the top this % of the height to the right */
    bool outline;       /* the unlit segments as a faint outline (Plus) */
    bool one_point;     /* only the decimal point, before the 10 Hz (Plus); else after MHz and kHz */
} seg_style_t;

static const seg_style_t *style;    /* the display being drawn */
static lv_coord_t         lean_y2;  /* its bottom: the lean is 0 there */

static lv_color_t faint(void) {
    return lv_color_mix(TC(text), TC(bg), 32);
}

static void lean(lv_point_t *p, int n) {
    for (int k = 0; k < n; k++) p[k].x += (lean_y2 - p[k].y) * style->lean_pct / 100;
}

/* One segment from p1 to p2, t thick, pointed at both ends, gap px short of them: filled in ink, or
 * outlined faintly when unlit */
static void stroke(lv_point_t p1, lv_point_t p2, lv_coord_t t, lv_coord_t gap, bool lit) {
    float len = sqrtf((float)(p2.x - p1.x) * (p2.x - p1.x) + (float)(p2.y - p1.y) * (p2.y - p1.y));

    if (len < 1.0f || (!lit && !style->outline)) return;
    float dx = (p2.x - p1.x) / len, dy = (p2.y - p1.y) / len, h = t / 2.0f;
    float ax = p1.x + dx * gap, ay = p1.y + dy * gap, bx = p2.x - dx * gap, by = p2.y - dy * gap;
    lv_point_t p[6] = {
        {ax, ay}, {ax + (dx - dy) * h, ay + (dy + dx) * h}, {bx - (dx + dy) * h, by - (dy - dx) * h},
        {bx, by}, {bx - (dx - dy) * h, by - (dy + dx) * h}, {ax + (dx + dy) * h, ay + (dy - dx) * h},
    };

    lean(p, 6);
    if (lit) {
        lv_draw_rect_dsc_t dsc;

        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = ink(true);
        lv_draw_polygon(ctx, &dsc, p, 6);
        return;
    }
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = faint();
    dsc.width = 1;
    for (int k = 0; k < 6; k++) lv_draw_line(ctx, &dsc, &p[k], &p[(k + 1) % 6]);
}

/* A digit in seven segments (bits a..g: top, top right, bottom right, bottom, bottom left, top
 * left, middle) in box */
static void seg7(const lv_area_t *box, uint8_t lit) {
    lv_coord_t w = lv_area_get_width(box);
    lv_coord_t t = LV_MAX(w / style->thick, 3), g = LV_MAX(1, t / 6);
    lv_coord_t l = box->x1 + t / 2, r = box->x2 - t / 2;
    lv_coord_t top = box->y1 + t / 2, m = (box->y1 + box->y2) / 2, b = box->y2 - t / 2;
    const lv_coord_t s[7][4] = {
        {l, top, r, top}, {r, top, r, m}, {r, m, r, b}, {l, b, r, b}, {l, m, l, b}, {l, top, l, m}, {l, m, r, m},
    };

    for (int i = 0; i < 7; i++)
        stroke((lv_point_t){s[i][0], s[i][1]}, (lv_point_t){s[i][2], s[i][3]}, t, g, lit & 1 << i);
}

/* Fourteen segments: a b c d e f as seven, g1 g2 the two halves of the middle, h i j the
 * diagonal, centre and diagonal over it, k l m under it */
enum { A = 1, B = 2, C = 4, D = 8, E = 16, F = 32, G1 = 64, G2 = 128, H = 256, I = 512, J = 1024, K = 2048,
       L = 4096, M = 8192 };

static uint16_t seg14_of(char c) {
    static const uint16_t digit[10] = {A | B | C | D | E | F, B | C, A | B | G1 | G2 | E | D,
                                       A | B | C | D | G2, F | G1 | G2 | B | C, A | F | G1 | G2 | C | D,
                                       A | F | E | D | C | G1 | G2, A | B | C, A | B | C | D | E | F | G1 | G2,
                                       A | B | C | D | F | G1 | G2};
    static const uint16_t alpha[26] = {
        A | B | C | E | F | G1 | G2, A | B | C | D | I | L | G2, A | D | E | F, A | B | C | D | I | L,
        A | D | E | F | G1 | G2, A | E | F | G1, A | C | D | E | F | G2, B | C | E | F | G1 | G2, A | D | I | L,
        B | C | D | E, E | F | G1 | J | M, D | E | F, B | C | E | F | H | J, B | C | E | F | H | M,
        A | B | C | D | E | F, A | B | E | F | G1 | G2, A | B | C | D | E | F | M, A | B | E | F | G1 | G2 | M,
        A | C | D | F | G1 | G2, A | I | L, B | C | D | E | F, E | F | K | J, B | C | E | F | K | M, H | J | K | M,
        H | J | L, A | D | J | K};

    if (c >= '0' && c <= '9') return digit[c - '0'];
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (c >= 'A' && c <= 'Z') return alpha[c - 'A'];
    switch (c) {
        case '-': return G1 | G2;
        case '?': return A | B | G2 | L;
        case '/': return J | K;
        case '=': return G1 | G2 | D;
        case '+': return G1 | G2 | I | L;
        case '<': return J | M;
        case '>': return H | K;
        case '\'': return I;
    }
    return 0;
}

static void seg14(const lv_area_t *box, uint16_t lit) {
    lv_coord_t w = lv_area_get_width(box);
    lv_coord_t t = LV_MAX(w / style->thick, 2), g = LV_MAX(1, t / 3);
    lv_coord_t l = box->x1 + t / 2, r = box->x2 - t / 2, c = (l + r) / 2;
    lv_coord_t top = box->y1 + t / 2, m = (box->y1 + box->y2) / 2, b = box->y2 - t / 2;
    const lv_coord_t s[14][4] = {
        {l, top, r, top}, {r, top, r, m}, {r, m, r, b}, {l, b, r, b}, {l, m, l, b}, {l, top, l, m},
        {l, m, c, m},     {c, m, r, m},   {l, top, c, m}, {c, top, c, m}, {r, top, c, m}, {l, b, c, m},
        {c, m, c, b},     {r, b, c, m},
    };

    for (int i = 0; i < 14; i++)
        stroke((lv_point_t){s[i][0], s[i][1]}, (lv_point_t){s[i][2], s[i][3]}, t, g, lit & 1 << i);
}

/* A frequency on 8 places, 10 Hz the last one: "  14.100.00", the points after places 2 and 5 */
static void freq_cells(int32_t f, char cells[9]) {
    char s[16];

    snprintf(s, sizeof(s), "%d%03d%02d", (int)(f / 1000000), (int)(f / 1000 % 1000), (int)(f / 10 % 100));
    snprintf(cells, 9, "%8.8s", s + LV_MAX(0, (int)strlen(s) - 8));
}

static const seg_style_t plus_a = {80, 5, 12, true, false}, plus_b = {80, 8, 12, true, false};
/* CW text: strokes a fifth of the width, a slight lean, no outlines (thicker, leaning and outlined it
 * could not be read) */
static const seg_style_t plus_text = {80, 5, 6, false, false};

/* A point t square at the bottom right of a place, leaning with it */
static void point(lv_coord_t x, lv_coord_t y2, lv_coord_t t) {
    lv_draw_rect_dsc_t dsc;
    lv_point_t         p[4] = {{x, y2 - t}, {x + t, y2 - t}, {x + t, y2}, {x, y2}};

    lean(p, 4);
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = ink(true);
    lv_draw_polygon(ctx, &dsc, p, 4);
}

/* The places of a display: x1..x2, y1..y2 in units, as st says */
static void digits(int x1, int x2, int y1, int y2, const char cells[9], bool fourteen, const seg_style_t *st) {
    static const uint8_t seven[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    int                  pitch = (x2 - x1) / 8, dw = pitch * st->dw_pct / 100;

    style   = st;
    lean_y2 = oy + UY(y2);
    for (int i = 0; i < 8; i++) {
        lv_area_t box = {ox + UX(x1 + i * pitch), oy + UY(y1), ox + UX(x1 + i * pitch + dw), oy + UY(y2)};
        char      ch  = cells[i];

        if (fourteen)
            seg14(&box, seg14_of(ch));
        else
            seg7(&box, ch >= '0' && ch <= '9' ? seven[ch - '0'] : 0);
        if ((st->one_point ? i == 5 : i == 2 || i == 5) && ch != ' ') {   /* the decimal point; MHz and kHz */
            lv_coord_t t = LV_MAX(UX(dw) / st->thick, fourteen ? 2 : 3);
            point(ox + UX(x1 + i * pitch + dw + (pitch - dw) / 2) - t / 2, oy + UY(y2), t);
        }
    }
}

/* The bar graph: 20 bars, S1..S9 on the first 9, +20..+60 on the other 11; in TX SWR on the first 7,
 * RF on the last 11. CWT (the KX3's CW tuning aid, on with the tuning setting in CW): the S-meter
 * keeps the first 9, the other 11 show the tone's offset from the pitch, zero beat in their middle
 * under the mark, a bar 10 Hz (as cw_tune_view: a lower tone to the right) */
#define BARS        20
#define BAR_X(i)    (51 + (i) * 17)
#define CWT_MID     14

static int s_bars(float db) {
    int n = db < S1 ? 0 : db <= S9 ? 1 + (int)((db - S1) / 6) : 9 + (int)((db - S9) * 11 / 60 + 0.5f);
    return LV_CLAMP(0, n, BARS);
}

static void bar_graph(const lcd_state_t *st) {
    static const struct { int bar; const char *s; } top[] = {
        {0, "S1"}, {2, "3"}, {4, "5"}, {6, "7"}, {8, "9"}, {12, "+20"}, {15, "40"}, {19, "60"}};
    bool tx = st->flags & 1, cwt = st->cwt && !tx;
    int  tune = CWT_MID - LV_CLAMP(-5, (st->cwt_hz + (st->cwt_hz < 0 ? -5 : 5)) / 10, 5);

    for (size_t i = 0; i < sizeof(top) / sizeof(top[0]); i++)
        text_c(BAR_X(top[i].bar) + 5, 73, top[i].s, true, &sony_18);
    text_c(283, 46, "CWT", cwt, &sony_18);
    tri(BAR_X(CWT_MID) + 5, 90, 7, 1, cwt);
    line(51, 98, BAR_X(BARS - 1) + 11, 98, 2, true);
    for (int i = 0; i < BARS; i++) {
        bool lit;

        line(BAR_X(i) + 5, 92, BAR_X(i) + 5, 98, 2, true);
        if (tx)
            lit = i < 7 ? i < st->swr_bars : i >= 9 && i - 9 < st->rf_bars;
        else if (cwt && i >= 9)
            lit = st->cwt_on && (i == tune || (LV_ABS(st->cwt_hz) <= 10 && LV_ABS(i - CWT_MID) <= 1));
        else
            lit = i < (cwt ? LV_MIN(st->bars, 9) : st->bars);
        rect(BAR_X(i), 108, BAR_X(i) + 11, 137, lit);
    }
    /* the TX scales: SWR and CMP under the first 7 bars, RF (the X6100's 0..10 W) and ALC under the last 11 */
    line(51, 150, BAR_X(6) + 11, 150, 2, true);
    line(BAR_X(9), 150, BAR_X(19) + 11, 150, 2, true);
    for (int i = 0; i < BARS; i++)
        if (i < 7 || i >= 9) line(BAR_X(i) + 5, 150, BAR_X(i) + 5, 156, 2, true);
    text(33, 176, "SWR", true, &sony_18);
    text_c(BAR_X(3) + 5, 176, "2", true, &sony_18);
    text_c(BAR_X(6) + 5, 176, "3", true, &sony_18);
    text(33, 202, "CMP", true, &sony_18);
    text_c(BAR_X(3) + 5, 202, "10", true, &sony_18);
    text_c(BAR_X(6) + 5, 202, "20", true, &sony_18);
    text(BAR_X(9) - 8, 176, "RF", true, &sony_18);
    text_c(BAR_X(14) + 5, 176, "5", true, &sony_18);
    text_c(BAR_X(19) + 5, 176, "10", true, &sony_18);
    text_c(BAR_X(12) + 5, 202, "ALC", true, &sony_18);
    line(BAR_X(9), 192, BAR_X(9), 204, 2, true);
    line(BAR_X(9), 204, BAR_X(10), 204, 2, true);
    line(BAR_X(14) + 11, 204, BAR_X(15), 204, 2, true);
    line(BAR_X(15), 192, BAR_X(15), 204, 2, true);
}

/* The filter graphic: 12 slanted stripes over 0..4 kHz of audio, the passband's lit; NTCH and the
 * arrows with the X6100's notch (DNF, set by hand) */
static void filter(const lcd_state_t *st) {
    int  lo = LV_MIN(abs(st->lo), abs(st->hi)), hi = LV_MAX(abs(st->lo), abs(st->hi));
    bool ntch = st->icons & 1 << I_NTCH;

    text(33, 236, "NTCH", ntch, &sony_18);
    tri(52, 266, 7, 2, ntch);
    tri(68, 266, 7, 3, ntch);
    for (int i = 0; i < 12; i++) {
        int        x = 117 + i * 18, hz = (i * 2 + 1) * 4000 / 24;
        lv_point_t p[4] = {{x + 6, 218}, {x + 18, 218}, {x + 12, 258}, {x, 258}};
        poly(p, 4, hz >= lo && hz <= hi);
    }
    line(113, 264, 330, 264, 3, true);
    line(93, 282, 113, 264, 3, true);
    line(330, 264, 348, 282, 3, true);
    tri(155, 274, 5, 0, true);
    tri(286, 274, 5, 0, true);
}

static void icons(const lcd_state_t *st) {
    bool on[16];
    int  x;

    for (int i = 0; i < 16; i++) on[i] = st->icons & 1 << i;
    text(403, 168, "VOX", on[I_VOX], &sony_20);
    text(683, 168, "ATU", on[I_ATU], &sony_20);
    boxed(758, 818, 168, "RIT", on[I_RIT]);
    boxed(836, 896, 168, "XIT", on[I_XIT]);
    boxed(1001, 1081, 168, "SPLT", on[I_SPLT]);

    /* AGC-S slow, AGC-F fast, AGC alone auto */
    x = text(403, 200, "AGC", st->agc != radio_agc_off, &sony_20);
    x = text(x, 200, "-", st->agc == radio_agc_slow || st->agc == radio_agc_fast, &sony_20);
    x = text(x, 200, "S", st->agc == radio_agc_slow, &sony_20);
    text(x, 200, "F", st->agc == radio_agc_fast, &sony_20);
    text(538, 200, "ATT", on[I_ATT], &sony_20);
    text(608, 200, "PRE", on[I_PRE], &sony_20);

    text(403, 237, "NB", on[I_NB], &sony_20);
    text(403, 268, "NR", on[I_NR], &sony_20);
    /* XFIL FL1: the one filter of the X6100, always */
    line(461, 237, 525, 237, 2, true);
    line(600, 237, 663, 237, 2, true);
    line(461, 237, 461, 249, 2, true);
    line(663, 237, 663, 249, 2, true);
    text_c(562, 237, "XFIL", true, &sony_18);
    text(463, 268, "FL1", true, &sony_16);
}

/* The current VFO on the big display, the other on the small one; the TX arrow to the one that sends */
static void vfo_icons(const lcd_state_t *st) {
    bool split = st->flags & 2, b = param_i_get(cfg_band_current_vfo) == RADIO_VFO_B;

    lock_icon(1140, 75, st->flags & 4);
    frame(1125, 100, 1155, 130, 15, true);
    text_c(1140, 115, b ? "B" : "A", true, &sony_18);
    tri(1140, 146, 8, 0, !split);
    text_c(1140, 168, "TX", true, &sony_20);
    tri(1140, 190, 8, 1, split);
    frame(1125, 208, 1155, 238, 15, true);
    text_c(1140, 223, b ? "A" : "B", true, &sony_18);
}

static void mode_icons(const lcd_state_t *st) {
    int8_t m = st->mode;
    int    x;

    text(1176, 47, "LSB", m == radio_mode_lsb || m == radio_mode_lsb_dig, &sony_20);
    text(1176, 77, "USB", m == radio_mode_usb || m == radio_mode_usb_dig, &sony_20);
    text(1176, 107, "CW", m == radio_mode_cw || m == radio_mode_cwr, &sony_20);
    text(1176, 138, "REV", m == radio_mode_cwr, &sony_20);
    text(1176, 168, "DATA", m == radio_mode_lsb_dig || m == radio_mode_usb_dig, &sony_20);
    x = text(1176, 198, "AM", m == radio_mode_am || m == radio_mode_sam, &sony_20);
    text(x, 198, "-S", m == radio_mode_sam, &sony_20);
    text(1176, 228, "FM", m == radio_mode_nfm || m == radio_mode_wfm, &sony_20);
}

static void plus_info(const lcd_state_t *st) {
    char s[32];
    int  x;

    snprintf(s, sizeof(s), "%d.%dW", st->pwr10 / 10, st->pwr10 % 10);
    x = text(30, 427, s, true, &sony_24);
    snprintf(s, sizeof(s), "IF %d.%dk", (int)(st->bw / 1000), (int)(st->bw / 100 % 10));
    text(x + 22, 427, s, true, &sony_24);
    /* the volts after the time, on the right */
    snprintf(s, sizeof(s), "%d.%dV", st->volts10 / 10, st->volts10 % 10);
    x = text_r(770, 427, s, true, &sony_24);
    snprintf(s, sizeof(s), "%02d:%02dZ", st->minute / 60, st->minute % 60);
    text_r(x - 22, 427, s, true, &sony_24);
}

/* CW: the strip's right half, the newest text in fourteen segments, ending at the right */
#define CW_CELLS    12

static void plus_cw(const lcd_state_t *st) {
    char       t[sizeof(cw_line)];
    const int  x1 = LCD_PLUS_STRIP_W / 2 + 8, x2 = 792, pitch = (x2 - x1) / CW_CELLS;
    const int  y1 = LCD_PLUS_STRIP_Y + (LCD_PLUS_STRIP_H - 56) / 2, y2 = y1 + 56;

    if (!st->cw) return;
    pthread_mutex_lock(&cw_mux);
    strcpy(t, cw_line);
    pthread_mutex_unlock(&cw_mux);
    frame(LCD_PLUS_STRIP_W / 2, LCD_PLUS_STRIP_Y, 799, LCD_PLUS_STRIP_Y + LCD_PLUS_STRIP_H - 1, 0, true);

    size_t n = strlen(t);
    const char *s = t + (n > CW_CELLS ? n - CW_CELLS : 0);
    int         first = CW_CELLS - (int)strlen(s);

    style   = &plus_text;
    lean_y2 = oy + y2;
    for (int i = 0; i < CW_CELLS; i++) {
        lv_area_t box = {ox + x1 + i * pitch, oy + y1, ox + x1 + i * pitch + pitch * 13 / 16, oy + y2};
        char      ch  = i < first ? ' ' : s[i - first];

        seg14(&box, seg14_of(ch));
        if (ch == '.' || ch == ',') point(box.x2 - 4, box.y2, 7);
    }
}

static void draw_plus(void) {
    char cells[9];

    kx = KX_X;
    ky = KX_Y;
    oy += LCD_PLUS_LCD_Y;   /* the display down, near the spectrum */
    bar_graph(&shown);
    filter(&shown);
    freq_cells(shown.fa, cells);
    digits(418, 1110, 42, 150, cells, false, &plus_a);   /* a little bigger than the KX3's 428..1078 x 37..133 */
    icons(&shown);
    freq_cells(shown.fb, cells);
    digits(685, 1078, 203, 275, cells, true, &plus_b);
    vfo_icons(&shown);
    mode_icons(&shown);

    kx = ky = 1;
    oy -= LCD_PLUS_LCD_Y;
    plus_info(&shown);
    plus_cw(&shown);
}

static void draw_cb(lv_event_t *e) {
    lv_area_t a;

    ctx = lv_event_get_draw_ctx(e);
    lv_obj_get_coords(panel, &a);
    ox = a.x1;
    oy = a.y1;
    draw_plus();
}

static void read_state(lcd_state_t *st) {
    bool  tx = radio_get_state() == RADIO_TX;
    float pwr = 0, swr = 1;

    memset(st, 0, sizeof(*st));
    st->fa   = cparam_i_get(cfg_fg_freq);
    st->fb   = cparam_i_get(cfg_bg_freq);
    st->lo   = cparam_i_get(cfg_cur_filter_low);
    st->hi   = cparam_i_get(cfg_cur_filter_high);
    st->mode = cparam_i_get(cfg_cur_mode);
    st->agc  = cparam_i_get(cfg_cur_agc);
    st->bars = s_bars(meter_get_db());

    float    ext, bat;
    uint8_t  cap;
    bool     charge;
    time_t   now = time(NULL);
    struct tm t;

    if (tx) {
        tx_info_get(&pwr, &swr);
        st->swr_bars = LV_CLAMP(0, (int)((swr - 1.0f) * 3 + 0.5f), 7);   /* 1.0 to 3.0 on 7 bars */
        st->rf_bars  = LV_CLAMP(0, (int)(pwr * 11 / 10 + 0.5f), 11);     /* 0 to 10 W on 11 bars */
    }
    clock_power_get(&ext, &bat, &cap, &charge);
    gmtime_r(&now, &t);
    st->sdb     = (int16_t)meter_get_db();
    st->swr10   = swr * 10 + 0.5f;
    st->pwr10   = (tx ? pwr : param_f_get(cfg_pwr)) * 10 + 0.5f;
    st->volts10 = (ext >= 3.0f ? ext : bat) * 10 + 0.5f;
    st->minute  = t.tm_hour * 60 + t.tm_min;
    st->bw      = cparam_i_get(cfg_cur_filter_bw);
    pthread_mutex_lock(&cw_mux);
    st->cw_seq = cw_seq;
    st->cw     = (st->mode == radio_mode_cw || st->mode == radio_mode_cwr) &&
                 (tx || (cw_line[0] && lv_tick_elaps(cw_ms) < CW_HOLD_MS));
    pthread_mutex_unlock(&cw_mux);
    int hz = 0;
    st->cwt    = param_i_get(cfg_cw_tune) && (st->mode == radio_mode_cw || st->mode == radio_mode_cwr);
    st->cwt_on = st->cwt && cw_tune_recent(&hz);
    st->cwt_hz = st->cwt_on ? LV_CLAMP(-99, hz, 99) : 0;
    st->flags = tx | (param_i_get(cfg_band_split) ? 2 : 0) | (main_screen_freq_locked() ? 4 : 0);
    st->icons = (param_i_get(cfg_vox_on) ? 1 << I_VOX : 0) | (param_i_get(cfg_atu_enabled) ? 1 << I_ATU : 0) |
                (param_i_get(cfg_rit) ? 1 << I_RIT : 0) | (param_i_get(cfg_xit) ? 1 << I_XIT : 0) |
                (param_i_get(cfg_band_split) ? 1 << I_SPLT : 0) | (cparam_i_get(cfg_cur_att) ? 1 << I_ATT : 0) |
                (cparam_i_get(cfg_cur_pre) ? 1 << I_PRE : 0) | (param_i_get(cfg_nb) ? 1 << I_NB : 0) |
                (param_i_get(cfg_nr) ? 1 << I_NR : 0) | (param_i_get(cfg_dnf) ? 1 << I_NTCH : 0);
}

/* ponytail: polled 10 times a second and drawn again on a change, subscriptions when it matters */
static void poll_cb(lv_timer_t *t) {
    lcd_state_t st;

    read_state(&st);
    if (memcmp(&st, &shown, sizeof(st))) {
        if (strip && st.cw != shown.cw) {   /* CW: the spectrum in the left half, the text in the right */
            lv_obj_set_width(strip, st.cw ? LCD_PLUS_STRIP_W / 2 : LCD_PLUS_STRIP_W);
            freq_scale_set_width(st.cw ? LCD_PLUS_STRIP_W / 2 : LCD_PLUS_STRIP_W);
        }
        shown = st;
        lv_obj_invalidate(panel);
    }
}

static void delete_cb(lv_event_t *e) {
    lv_timer_del(timer);
    timer = NULL;
    panel = NULL;
    strip = NULL;
}

lv_obj_t *lcd_panel_init(lv_obj_t *parent, lv_coord_t height) {
    panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_size(panel, 800, height);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, TC(bg), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(panel, delete_cb, LV_EVENT_DELETE, NULL);
    read_state(&shown);
    timer = lv_timer_create(poll_cb, 100, NULL);
    return panel;
}

void lcd_panel_set_strip(lv_obj_t *spectrum) {
    strip = spectrum;
}

/* Any thread: the CW decoder's text, kept to the last line's worth */
void lcd_panel_cw_text(const char *text) {
    pthread_mutex_lock(&cw_mux);
    size_t n = strlen(cw_line), add = strlen(text);
    if (n + add >= sizeof(cw_line)) {
        size_t drop = n + add - sizeof(cw_line) + 1;
        if (drop > n) drop = n;
        memmove(cw_line, cw_line + drop, n - drop + 1);
        n -= drop;
    }
    snprintf(cw_line + n, sizeof(cw_line) - n, "%s", text);
    for (char *c = cw_line + n; *c; c++) {
        if (*c == '\n' || *c == '\r') *c = ' ';
        if (!strncmp(c, "<?>", 3)) {   /* the decoder's unknown sign: one place */
            *c = '?';
            memmove(c + 1, c + 3, strlen(c + 3) + 1);
        }
    }
    cw_seq++;
    cw_ms = lv_tick_get();
    pthread_mutex_unlock(&cw_mux);
}

void lcd_panel_levels(float *min, float *max) {
    /* ponytail: by eye on one radio, a setting if other bands or radios need another */
    float span = *max - *min;

    *min -= 0.25f * span;
    *max -= 0.35f * span;
}

void lcd_panel_squeeze(uint32_t *strip, int h, int points, int w, uint32_t bg) {
    if (w <= 0 || w >= points) return;
    /* each column x takes the columns x * points / w up to the next: in place, left to right (they
     * are never left of x), a pixel the first of them that is not bg, so the trace stays joined */
    for (int x = 0; x < w; x++) {
        uint32_t *dst = &strip[(points - 1 - x) * h];
        int       a = x * points / w, b = LV_MAX((x + 1) * points / w, a + 1);

        for (int y = 0; y < h; y++) {
            uint32_t v = bg;

            for (int c = a; c < b && v == bg; c++) v = strip[(points - 1 - c) * h + y];
            dst[y] = v;
        }
    }
    for (int x = w; x < points; x++) {
        uint32_t *col = &strip[(points - 1 - x) * h];

        for (int y = 0; y < h; y++) col[y] = bg;
    }
}
