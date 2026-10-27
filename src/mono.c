/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The 1bit theme's drawing: hooks in LVGL's software renderer that leave every pixel it draws
 *  ink or paper, at alpha 0 or 255, whatever the code asked for. The theme gives the screen its
 *  look (ink and paper roles, the pixel font, the icons); this keeps it strictly two-coloured
 *  where the code draws anti-aliased edges, see-through boxes, shadows, gradients, pictures or
 *  colours of its own.
 *
 *  Everything LVGL draws ends in the blend; the rects and the letters are hooked too:
 *  - coverage (anti-aliased edges, glyphs at 4 bpp, image alpha) is in or out: in when the mask
 *    is at least half of the opacity (a label's opacity is already in its glyph mask);
 *  - opacity: under 25 % nothing, up to 44 % a 50 % checkerboard (by screen pixel), more is solid.
 *    See-through panels exist so text stays readable over the waterfall: in 1-bit that is a solid
 *    box; only faint decoration becomes a stipple. A fade dissolves in three steps;
 *  - a fill colour: the nearer of ink and paper; no blend modes, no shadows, no gradients (their
 *    first colour), square corners (dots and round pills stay round);
 *  - pictures and layers: their ink and paper pixels as they are, the rest ordered-dithered by
 *    screen position (SSTV, WeFax, flags, Field Ops tiles read as dot density);
 *  - text in ink on a box that is mostly ink is drawn in paper: an "on" badge with the theme's
 *    text colour on it comes out inverted. Never the other way (the spot calls' paper halo).
 *  And the widgets LVGL's default theme styles (switches, sliders, bars, check boxes, the keyboard,
 *  scroll bars, focus outlines) get ink and paper styles of their own.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#include "mono.h"
#include "theme.h"

#include "lvgl/src/core/lv_refr.h"
#include "lvgl/src/draw/sw/lv_draw_sw.h"

#define CHUNK_PX    8192    /* pixels per blend when the mask or the picture is made here */
#define OPA_NONE    64      /* under 25 %: not drawn */
#define OPA_SOLID   112     /* from 44 %: solid; between the two: a 50 % stipple */

static bool       on;
static lv_color_t ink, paper;
static uint32_t   ink24, paper24, paper_luma;

static uint32_t luma(lv_color_t c) {
    return (c.ch.red * 77 + c.ch.green * 150 + c.ch.blue * 29) >> 8;
}

lv_color_t mono_snap(lv_color_t c) {
    return luma(c) >= 128 ? paper : ink;
}

/* A picture's pixel: ink and paper as they are, other colours dithered by their lightness against the paper */
static inline lv_color_t picture_px(lv_color_t c, int x, int y) {
    uint32_t rgb = c.full & 0xffffff;

    if (rgb == ink24) return ink;
    if (rgb == paper24) return paper;
    uint32_t l = luma(c) * 255 / paper_luma;

    return mono_dither(255 - (l > 255 ? 255 : l), x, y) ? ink : paper;
}

static void mono_blend(lv_draw_ctx_t *ctx, const lv_draw_sw_blend_dsc_t *dsc) {
    lv_area_t area;
    lv_opa_t *mask = dsc->mask_buf;
    lv_opa_t  opa  = dsc->opa;

    if (!_lv_area_intersect(&area, dsc->blend_area, ctx->clip_area)) return;
    if (mask && dsc->mask_res == LV_DRAW_MASK_RES_TRANSP) return;
    if (dsc->mask_res == LV_DRAW_MASK_RES_FULL_COVER) mask = NULL;
    if (opa < OPA_NONE) return;

    bool                   stipple = opa < OPA_SOLID;
    lv_coord_t             w       = lv_area_get_width(&area);
    lv_coord_t             mw      = mask ? lv_area_get_width(dsc->mask_area) : 0;
    lv_draw_sw_blend_dsc_t d       = *dsc;

    d.opa        = LV_OPA_COVER;
    d.blend_mode = LV_BLEND_MODE_NORMAL;
    d.color      = mono_snap(dsc->color);

    /* coverage in or out, over the part that is drawn (the mask is the caller's scratch buffer,
     * as with LVGL's own antialiasing = 0) */
    if (mask) {
        for (lv_coord_t y = area.y1; y <= area.y2; y++) {
            lv_opa_t *m = mask + (y - dsc->mask_area->y1) * mw + (area.x1 - dsc->mask_area->x1);

            for (lv_coord_t i = 0; i < w; i++) m[i] = m[i] * 2 >= opa ? LV_OPA_COVER : LV_OPA_TRANSP;
        }
    }
    if (!dsc->src_buf && !stipple) {    /* a plain fill: most calls */
        d.mask_buf = mask;
        d.mask_res = mask ? LV_DRAW_MASK_RES_CHANGED : LV_DRAW_MASK_RES_FULL_COVER;
        lv_draw_sw_blend_basic(ctx, &d);
        return;
    }

    /* the mask (stipple) or the picture made here, a few rows at a time */
    lv_coord_t  rows = LV_MAX(1, CHUNK_PX / w);
    lv_opa_t   *mbuf = mask || stipple ? lv_mem_buf_get(w * rows) : NULL;
    lv_color_t *cbuf = dsc->src_buf ? lv_mem_buf_get(w * rows * sizeof(lv_color_t)) : NULL;
    lv_coord_t  sw   = lv_area_get_width(dsc->blend_area);

    for (lv_coord_t y0 = area.y1; y0 <= area.y2; y0 += rows) {
        lv_area_t part = {area.x1, y0, area.x2, LV_MIN(y0 + rows - 1, area.y2)};

        for (lv_coord_t y = part.y1; y <= part.y2; y++) {
            lv_opa_t         *mo = mbuf ? mbuf + (y - y0) * w : NULL;
            lv_color_t       *co = cbuf ? cbuf + (y - y0) * w : NULL;
            const lv_opa_t   *mi = mask ? mask + (y - dsc->mask_area->y1) * mw + (area.x1 - dsc->mask_area->x1) : NULL;
            const lv_color_t *ci = dsc->src_buf ? dsc->src_buf + (y - dsc->blend_area->y1) * sw +
                                                      (area.x1 - dsc->blend_area->x1)
                                                : NULL;

            for (lv_coord_t i = 0; i < w; i++) {
                int x = area.x1 + i;

                if (mo) mo[i] = (mi ? mi[i] : LV_OPA_COVER) & (stipple && ((x + y) & 1) ? 0 : 0xff);
                if (co) co[i] = picture_px(ci[i], x, y);
            }
        }
        d.blend_area = &part;
        d.mask_area  = &part;
        d.mask_buf   = mbuf;
        d.mask_res   = mbuf ? LV_DRAW_MASK_RES_CHANGED : LV_DRAW_MASK_RES_FULL_COVER;
        d.src_buf    = cbuf;
        lv_draw_sw_blend_basic(ctx, &d);
    }
    if (mbuf) lv_mem_buf_release(mbuf);
    if (cbuf) lv_mem_buf_release(cbuf);
}

/* No shadows, no gradients (their first colour), square corners but for dots and round pills */
static void mono_draw_rect(lv_draw_ctx_t *ctx, const lv_draw_rect_dsc_t *dsc, const lv_area_t *coords) {
    lv_draw_rect_dsc_t d = *dsc;
    lv_coord_t         s = LV_MIN(lv_area_get_width(coords), lv_area_get_height(coords));

    d.shadow_opa = LV_OPA_TRANSP;
    if (d.bg_grad.dir != LV_GRAD_DIR_NONE) {
        d.bg_color    = d.bg_grad.stops[0].color;
        d.bg_grad.dir = LV_GRAD_DIR_NONE;
    }
    if (d.radius != LV_RADIUS_CIRCLE && d.radius * 2 < s) d.radius = 0;
    lv_draw_sw_rect(ctx, &d, coords);
}

/* Ink text over a ground that is mostly ink is drawn in paper */
static void mono_draw_letter(lv_draw_ctx_t *ctx, const lv_draw_label_dsc_t *dsc, const lv_point_t *pos,
                             uint32_t letter) {
    lv_draw_label_dsc_t d = *dsc;
    lv_font_glyph_dsc_t g;

    d.color = mono_snap(dsc->color);
    if (d.color.full == ink.full && lv_font_get_glyph_dsc(dsc->font, &g, letter, '\0') && g.box_w && g.box_h) {
        lv_area_t box = {pos->x + g.ofs_x, pos->y + (dsc->font->line_height - dsc->font->base_line) - g.box_h - g.ofs_y};
        lv_area_t in;

        box.x2 = box.x1 + g.box_w - 1;
        box.y2 = box.y1 + g.box_h - 1;
        if (_lv_area_intersect(&in, &box, ctx->clip_area) && _lv_area_intersect(&in, &in, ctx->buf_area)) {
            lv_disp_t        *disp   = _lv_refr_get_disp_refreshing();
            bool              transp = disp && disp->driver->screen_transp;
            lv_coord_t        stride = lv_area_get_width(ctx->buf_area);
            const lv_color_t *buf    = ctx->buf;
            uint32_t          n_ink = 0, n_paper = 0;

            for (lv_coord_t y = in.y1; y <= in.y2; y++) {
                const lv_color_t *p = buf + (y - ctx->buf_area->y1) * stride + (in.x1 - ctx->buf_area->x1);

                for (lv_coord_t x = in.x1; x <= in.x2; x++, p++) {
                    uint8_t a = p->ch.alpha;

                    if (transp ? a != 0xff : a == 0) continue;   /* see-through: neither */
                    if (luma(*p) < 128) n_ink++;
                    else n_paper++;
                }
            }
            if (n_ink > n_paper) d.color = paper;
        }
    }
    lv_draw_sw_letter(ctx, &d, pos, letter);
}

static void hooks(lv_draw_ctx_t *ctx, bool mono) {
    lv_draw_sw_ctx_t *sw = (lv_draw_sw_ctx_t *)ctx;

    if (!ctx) return;
    sw->blend                 = mono ? mono_blend : lv_draw_sw_blend_basic;
    sw->base_draw.draw_rect   = mono ? mono_draw_rect : lv_draw_sw_rect;
    sw->base_draw.draw_letter = mono ? mono_draw_letter : lv_draw_sw_letter;
}

void mono_draw_ctx_init(lv_disp_drv_t *drv, lv_draw_ctx_t *ctx) {
    lv_draw_sw_init_ctx(drv, ctx);
    if (on) hooks(ctx, true);
}

/* ---- LVGL's widget theme in ink and paper ---- */

static lv_theme_t mono_theme;
static lv_style_t st_scroll, st_focus, st_track, st_ind, st_knob, st_item, st_item_on;

static void mono_theme_apply(lv_theme_t *th, lv_obj_t *obj) {
    (void)th;
    lv_obj_add_style(obj, &st_scroll, LV_PART_SCROLLBAR);
    lv_obj_add_style(obj, &st_scroll, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);
    lv_obj_add_style(obj, &st_focus, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(obj, &st_focus, LV_STATE_EDITED);
    if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &st_track, 0);
        lv_obj_add_style(obj, &st_ind, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &st_knob, LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_slider_class) || lv_obj_check_type(obj, &lv_bar_class)) {
        lv_obj_add_style(obj, &st_track, 0);
        lv_obj_add_style(obj, &st_ind, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &st_knob, LV_PART_KNOB);
    } else if (lv_obj_has_class(obj, &lv_btnmatrix_class)) {   /* the keyboard too */
        lv_obj_add_style(obj, &st_item, LV_PART_ITEMS);
        lv_obj_add_style(obj, &st_item_on, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &st_item_on, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_add_style(obj, &st_item_on, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_add_style(obj, &st_knob, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &st_ind, LV_PART_INDICATOR | LV_STATE_CHECKED);
    }
}

/* The styles again at each mono_set(true): ink and paper can change with the theme file */
static void theme_styles(void) {
    lv_style_t *all[] = {&st_scroll, &st_focus, &st_track, &st_ind, &st_knob, &st_item, &st_item_on};

    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) lv_style_reset(all[i]);
    lv_style_set_bg_color(&st_scroll, ink);
    lv_style_set_bg_opa(&st_scroll, LV_OPA_COVER);
    lv_style_set_radius(&st_scroll, 0);
    lv_style_set_outline_color(&st_focus, ink);
    lv_style_set_outline_opa(&st_focus, LV_OPA_COVER);
    lv_style_set_bg_color(&st_track, paper);
    lv_style_set_bg_opa(&st_track, LV_OPA_COVER);
    lv_style_set_border_width(&st_track, 1);
    lv_style_set_border_color(&st_track, ink);
    lv_style_set_border_opa(&st_track, LV_OPA_COVER);
    lv_style_set_bg_color(&st_ind, ink);
    lv_style_set_bg_opa(&st_ind, LV_OPA_COVER);
    lv_style_set_bg_color(&st_knob, paper);
    lv_style_set_bg_opa(&st_knob, LV_OPA_COVER);
    lv_style_set_border_width(&st_knob, 1);
    lv_style_set_border_color(&st_knob, ink);
    lv_style_set_border_opa(&st_knob, LV_OPA_COVER);
    lv_style_set_shadow_width(&st_knob, 0);
    lv_style_set_bg_color(&st_item, paper);
    lv_style_set_bg_opa(&st_item, LV_OPA_COVER);
    lv_style_set_border_width(&st_item, 1);
    lv_style_set_border_color(&st_item, ink);
    lv_style_set_border_opa(&st_item, LV_OPA_COVER);
    lv_style_set_shadow_width(&st_item, 0);
    lv_style_set_text_color(&st_item, ink);
    lv_style_set_bg_color(&st_item_on, ink);
    lv_style_set_bg_opa(&st_item_on, LV_OPA_COVER);
}

void mono_set(bool mono) {
    lv_disp_t *disp = lv_disp_get_default();

    on      = mono;
    ink     = lv_color_hex(theme_shape.ink);
    paper   = lv_color_hex(theme_shape.paper);
    ink24   = theme_shape.ink & 0xffffff;
    paper24 = theme_shape.paper & 0xffffff;
    paper_luma = LV_MAX(1, luma(paper));
    if (!disp) return;
    hooks(disp->driver->draw_ctx, mono);
    disp->driver->antialiasing = !mono;     /* nearest pixel in transformed pictures */

    lv_theme_t *def = lv_theme_default_get();

    if (mono && def) {
        theme_styles();
        mono_theme = *def;
        lv_theme_set_parent(&mono_theme, def);
        lv_theme_set_apply_cb(&mono_theme, mono_theme_apply);
        lv_disp_set_theme(disp, &mono_theme);
    } else if (def) {
        lv_disp_set_theme(disp, def);
    }
}
