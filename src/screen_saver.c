/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the screensavers (see screen_saver.h)
 *
 *  Matrix rain: columns of characters fall, each at its own speed, a pale head and a trail that fades, the
 *  characters mirrored as in the film. The rain of VFO-Knob's screensaver (components/ui/saver.c, by the same
 *  author), which draws straight to the knob's panel; here it is an LVGL object, drawn as the rest of the GUI.
 *
 *  One object over the whole screen, opaque and the last child of the active screen: LVGL draws an area from the
 *  topmost object that covers it, so nothing of the GUI under it is drawn. The top layer (messages, popups) is
 *  hidden meanwhile, and screen_light.c has LVGL's invalidation off: what changes in the GUI under it draws nothing.
 *  The trail is bright behind the head and fades over its last FADE cells, so a step changes only the cells at the
 *  head and those at the tail: every FRAME_MS each column that moved invalidates those two, and only they are drawn,
 *  some 90 000 pixels in 40 areas a frame on the PC build (the whole trail a step was 160 000). The waterfall and
 *  the spectrum stop drawing their pictures meanwhile (not seen: backlight_seen()), their data goes on. The glyphs
 *  are made once from the font (fonts/matrix_20.c) as coverage masks, mirrored, and blended within their ink in the
 *  theme's colours (saver, saver_head on saver_bg) by LVGL's own blend: the 1bit theme's hooks (mono.c) make them
 *  ink and paper as everything else, the fade a stipple.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "screen_saver.h"

#include "board.h"
#include "params/params.h"
#include "screen_idle.h"
#include "screen_light.h"
#include "theme.h"

#include "lvgl/lvgl.h"
#include "lvgl/src/draw/sw/lv_draw_sw.h"

#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(matrix_20);

#define FRAME_MS 66         /* 15 frames a second */
#define CELL_W   16         /* 50 columns of 20 rows on 800 x 480 */
#define CELL_H   24
#define DISP_MAX LV_MAX(BOARD_DISP_W, BOARD_DISP_H)    /* the walkie screen is portrait */
#define MAX_COLS (DISP_MAX / CELL_W)
#define MAX_ROWS (DISP_MAX / CELL_H)
#define FADE     4          /* the trail's last cells, fading to the ground */

/* The film's set: the half-width katakana, the digits and a few signs */
static const char SIGNS[] = ":.=*+-<>|";
#define KANA   (0xFF9D - 0xFF66 + 1)
#define GLYPHS (KANA + 10 + (int)sizeof(SIGNS) - 1)

typedef struct {
    int16_t  head;      /* the head's row; below 0, still above the screen */
    uint8_t  len;       /* the cells of its trail */
    uint16_t period;    /* ms a step */
    uint32_t due;
    uint8_t  glyph[MAX_ROWS];
} column_t;

static lv_obj_t   *saver;
static lv_timer_t *frames;      /* the screensaver on */
static uint8_t     slow = 1;    /* the step times' factor, from params.screen_saver_speed at the rain's start */
static bool        top_hidden;  /* the top layer hidden by us */
static lv_coord_t  width, height;
static int         cols, rows;
static column_t    col[MAX_COLS];
static uint8_t    *atlas;       /* each glyph's coverage, mirrored, CELL_W x CELL_H */
static lv_area_t   ink[GLYPHS]; /* where in its cell each glyph has any */
static lv_opa_t    mask[CELL_W * CELL_H];  /* the part of a glyph blended, packed; the blend may write in it (mono.c) */

/* xorshift32: the rain's randomness */
static uint32_t rng = 0x2545F491u;

static uint32_t rnd(uint32_t n) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return n ? rng % n : 0;
}

static uint32_t glyph_code(int g) {
    if (g < KANA) {
        return 0xFF66 + g;
    }
    g -= KANA;
    return g < 10 ? (uint32_t)('0' + g) : (uint32_t)SIGNS[g - 10];
}

/* Each glyph once, from the font's 4 bpp bitmap: the set on one baseline in the middle of the cell, centred
 * across, mirrored */
static bool atlas_make(void) {
    lv_font_glyph_dsc_t g;
    int                 top = 0, bottom = 0;    /* the set's ink above and below the baseline */

    if (atlas) {
        return true;
    }
    for (int i = 0; i < GLYPHS; i++) {
        if (!lv_font_get_glyph_dsc(&matrix_20, &g, glyph_code(i), 0) || g.bpp != 4) {
            LV_LOG_ERROR("Screensaver: no glyph %04x at 4 bpp", (unsigned)glyph_code(i));
            return false;
        }
        top    = LV_MAX(top, g.box_h + g.ofs_y);
        bottom = LV_MIN(bottom, g.ofs_y);
    }
    atlas = calloc(GLYPHS, CELL_W * CELL_H);
    if (!atlas) {
        return false;
    }

    int base = (CELL_H - (top - bottom)) / 2 + top;     /* the baseline's row in the cell */

    for (int i = 0; i < GLYPHS; i++) {
        const uint8_t *bmp = lv_font_get_glyph_bitmap(&matrix_20, glyph_code(i));
        uint8_t       *o   = atlas + i * CELL_W * CELL_H;
        uint32_t       bit = 0;

        lv_font_get_glyph_dsc(&matrix_20, &g, glyph_code(i), 0);
        int x0 = (CELL_W - g.box_w) / 2, y0 = base - (g.box_h + g.ofs_y);

        ink[i] = (lv_area_t){CELL_W, CELL_H, -1, -1};
        for (int y = 0; y < g.box_h; y++) {
            for (int x = 0; x < g.box_w; x++, bit += 4) {
                int cx = CELL_W - 1 - (x0 + x), cy = y0 + y;    /* mirrored */

                if (bmp && cx >= 0 && cx < CELL_W && cy >= 0 && cy < CELL_H) {
                    o[cy * CELL_W + cx] = (bmp[bit >> 3] >> (bit & 4 ? 0 : 4) & 0xf) * 17;
                    if (o[cy * CELL_W + cx]) {
                        ink[i].x1 = LV_MIN(ink[i].x1, cx);
                        ink[i].y1 = LV_MIN(ink[i].y1, cy);
                        ink[i].x2 = LV_MAX(ink[i].x2, cx);
                        ink[i].y2 = LV_MAX(ink[i].y2, cy);
                    }
                }
            }
        }
    }
    return true;
}

static void column_reset(column_t *c, uint32_t now, bool first) {
    c->head   = (int16_t)-(int)rnd(first ? rows * 2 : rows / 2);
    c->len    = (uint8_t)(FADE + 3 + rnd(10));
    c->period = (uint16_t)((45 + rnd(100)) * slow);
    c->due    = now + rnd(c->period);
    for (int r = 0; r < rows; r++) {
        c->glyph[r] = (uint8_t)rnd(GLYPHS);
    }
}

/* A trail cell d rows behind the head (1 to len): bright, then fading over the last FADE to a sixth */
static lv_opa_t trail_opa(const column_t *c, int d) {
    int f = d - (c->len - FADE);

    return f <= 0 ? LV_OPA_COVER : (lv_opa_t)(255 - f * 215 / FADE);
}

/* One glyph in its cell, the colour at opa over what is there; only where the glyph has ink, in the area drawn.
 * The mask is that area's, row after row: LVGL 8.3's blend into the radios' transparent screen (fill_argb, opa
 * at its top) does not step over the rest of a mask wider than the area, and drew each row from the wrong place */
static void cell(lv_draw_ctx_t *ctx, lv_coord_t x, lv_coord_t y, uint8_t glyph, lv_color_t color, lv_opa_t opa) {
    lv_area_t              g = {x + ink[glyph].x1, y + ink[glyph].y1, x + ink[glyph].x2, y + ink[glyph].y2};
    lv_area_t              b;
    lv_draw_sw_blend_dsc_t d;

    if (g.x1 > g.x2 || !_lv_area_intersect(&b, &g, ctx->clip_area)) {
        return;
    }
    lv_coord_t     w   = lv_area_get_width(&b);
    const uint8_t *src = atlas + glyph * CELL_W * CELL_H + (b.y1 - y) * CELL_W + (b.x1 - x);

    for (lv_coord_t r = 0; r <= b.y2 - b.y1; r++) {
        memcpy(mask + r * w, src + r * CELL_W, w);
    }
    memset(&d, 0, sizeof(d));
    d.blend_area = &b;
    d.mask_area  = &b;
    d.mask_buf   = mask;
    d.mask_res   = LV_DRAW_MASK_RES_CHANGED;
    d.color      = color;
    d.opa        = opa;
    d.blend_mode = LV_BLEND_MODE_NORMAL;
    lv_draw_sw_blend(ctx, &d);
}

/* Over the ground the object's style drew: the cells of the trails in the area drawn. The head pale, the trail
 * bright and fading at its end, the cell after it the ground */
static void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t   *ctx  = lv_event_get_draw_ctx(e);
    const lv_area_t *clip = ctx->clip_area;
    lv_obj_t        *o    = lv_event_get_target(e);
    lv_coord_t       x0 = o->coords.x1, y0 = o->coords.y1;
    lv_color_t       rain = TC(saver), head = TC(saver_head);
    int              c0 = LV_MAX((clip->x1 - x0) / CELL_W, 0), c1 = LV_MIN((clip->x2 - x0) / CELL_W, cols - 1);
    int              r0 = LV_MAX((clip->y1 - y0) / CELL_H, 0), r1 = LV_MIN((clip->y2 - y0) / CELL_H, rows - 1);

    for (int i = c0; i <= c1; i++) {
        const column_t *c = &col[i];

        for (int r = LV_MAX(r0, c->head - c->len); r <= LV_MIN(r1, c->head); r++) {
            int d = c->head - r;

            cell(ctx, x0 + i * CELL_W, y0 + r * CELL_H, c->glyph[r], d ? rain : head,
                 d ? trail_opa(c, d) : LV_OPA_COVER);
        }
    }
}

/* The pointer on the screensaver: it only wakes the screen, the GUI under it does not see it */
static void press_cb(lv_event_t *e) {
    backlight_tick();
}

static void delete_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == saver) {
        saver = NULL;       /* with the screen it was on: made again at the next frame */
    }
}

/* The object over the active screen, its last child, as big as the display. A theme change builds the main screen
 * again (its children deleted, this one too), the walkie mode loads a screen of its own (portrait): checked at each
 * frame. True when it is all to be drawn again */
static bool place(uint32_t now) {
    lv_obj_t  *scr = lv_scr_act();
    lv_coord_t w = lv_disp_get_hor_res(NULL), h = lv_disp_get_ver_res(NULL);
    bool       all = false;

    if (!saver) {
        saver = lv_obj_create(scr);
        lv_obj_remove_style_all(saver);
        lv_obj_set_style_bg_color(saver, TC(saver_bg), 0);
        lv_obj_set_style_bg_opa(saver, LV_OPA_COVER, 0);
        lv_obj_clear_flag(saver, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(saver, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(saver, press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(saver, delete_cb, LV_EVENT_DELETE, NULL);
        width = height = 0;
        all = true;
    } else if (lv_obj_get_parent(saver) != scr) {
        lv_obj_set_parent(saver, scr);
        all = true;
    }
    if (lv_obj_get_index(saver) + 1 != lv_obj_get_child_cnt(scr)) {
        lv_obj_move_foreground(saver);
        all = true;
    }
    if (w != width || h != height) {
        width = w;
        height = h;
        lv_obj_set_pos(saver, 0, 0);
        lv_obj_set_size(saver, w, h);
        lv_obj_update_layout(saver);
        cols = LV_MIN(w / CELL_W, MAX_COLS);
        rows = LV_MIN(h / CELL_H, MAX_ROWS);
        for (int i = 0; i < cols; i++) {
            column_reset(&col[i], now, true);
        }
        all = true;
    }
    if (!lv_obj_has_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
        top_hidden = true;
    }
    return all;
}

/* Rows r0 to r1 of column i to be drawn again, those on the screen */
static void invalidate(int i, int r0, int r1) {
    r0 = LV_MAX(r0, 0);
    r1 = LV_MIN(r1, rows - 1);
    if (r0 <= r1) {
        lv_area_t a = {saver->coords.x1 + i * CELL_W, saver->coords.y1 + r0 * CELL_H,
                       saver->coords.x1 + i * CELL_W + CELL_W - 1, saver->coords.y1 + r1 * CELL_H + CELL_H - 1};

        lv_obj_invalidate_area(saver, &a);
    }
}

/* The columns that are due move a row, two at most. What changed: at the head, the rows it moved over and the one
 * it left (pale to bright); at the tail, the rows it left (to the ground) and the fading ones, now a row further.
 * Two areas a column, one when they meet; the bright middle stays as it is */
static void frame_cb(lv_timer_t *t) {
    uint32_t now = lv_tick_get();
    bool     all = place(now);

    lv_disp_enable_invalidation(NULL, true);    /* for the rain alone: off again below */
    if (all) {
        lv_obj_invalidate(saver);
    }
    for (int i = 0; i < cols; i++) {
        column_t *c    = &col[i];
        int       from = c->head;
        int       k    = 0;

        for (; k < 2 && (int32_t)(now - c->due) >= 0; k++) {
            c->due += c->period;
            c->head++;
            if (c->head >= 0 && c->head < rows) {
                c->glyph[c->head] = (uint8_t)rnd(GLYPHS);
            }
            /* Now and then a character turns over, where the rows are drawn again anyway: just behind the head or
             * in the fading end */
            int tr = rnd(2) ? c->head - 1 : c->head - c->len + (int)rnd(FADE);

            if (tr >= 0 && tr < rows && !rnd(3)) {
                c->glyph[tr] = (uint8_t)rnd(GLYPHS);
            }
        }
        if ((int32_t)(now - c->due) >= 0) {
            c->due = now + c->period;   /* fell behind: catch up */
        }
        if (!k || all) {
            continue;
        }
        int tail0 = from - c->len, tail1 = c->head - c->len + FADE - 1;

        if (tail1 + 1 >= from) {
            invalidate(i, tail0, c->head);
        } else {
            invalidate(i, tail0, tail1);
            invalidate(i, from, c->head);
        }
        if (c->head - c->len >= rows) {
            column_reset(c, now, false);    /* all of it below the screen: again from the top */
        }
    }
    lv_disp_enable_invalidation(NULL, false);
}

void screen_saver_show(uint8_t kind) {
    if (frames || kind != SCREEN_SAVER_MATRIX || !atlas_make()) {
        return;
    }
    rng ^= lv_tick_get();
    slow = 1 + LV_MIN(params.screen_saver_speed.x, SAVER_SPEED_LAST);   /* Normal, Slow, Very slow: 1, 2, 3 (#200) */
    width = height = 0;     /* a new rain */
    frames = lv_timer_create(frame_cb, FRAME_MS, NULL);
    frame_cb(frames);
}

/* The object hidden at once and deleted after the event that may have called this (a press on it) */
void screen_saver_hide(void) {
    if (!frames) {
        return;
    }
    lv_timer_del(frames);
    frames = NULL;
    if (saver) {
        lv_obj_add_flag(saver, LV_OBJ_FLAG_HIDDEN);
        lv_obj_del_async(saver);
        saver = NULL;
    }
    if (top_hidden) {
        lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
        top_hidden = false;
    }
}
