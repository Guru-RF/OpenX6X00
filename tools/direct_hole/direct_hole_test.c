/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of direct mode's see-through hole (direct_hole_cb, src/waterfall_view.c, #177). On the radio
 *  (DRM planes) the spectrum's and the waterfall's areas of the GUI are drawn fully transparent (alpha 0), so the
 *  strip and the waterfall on the primary plane under the GUI show. With Settings > Display > Spectrum off,
 *  spectrum_layout() (screen_main.c) parks the spectrum under the top bars at LV_OPA_TRANSP, and the hole was still
 *  made there: the bars' gaps and round corners showed the primary plane, black, never written there, in place of the
 *  theme's background. The PC build has no direct mode (USE_DRM=0) and never adds the callback, so this is the only
 *  place off the radio it runs.
 *
 *  An object made as spectrum_init() makes it in direct mode (transparent background, the callback on
 *  LV_EVENT_DRAW_MAIN_END), laid out by the real LVGL where spectrum_layout() puts it, and the event sent as
 *  lv_obj_redraw() sends it, with a draw buffer the size of the screen filled with a known value. Checked: on, its
 *  area zeroed and nothing around it; only what is in the clip area (a refresh across its edge); nothing for a clip
 *  area beside it; off (spectrum_layout's lv_obj_set_style_opa and move under the bars), the buffer untouched; on
 *  again (its lv_obj_remove_local_style_prop and move back), zeroed again. The waterfall's object, whose opacity is
 *  never set, is the "on" case. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "lvgl/lvgl.h"
#include "waterfall_view.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define W       800
#define H       480
#define FILL    0xab        /* the GUI's pixels before the hole, not 0 */

/* spectrum_layout() with the standard top bar (66) and frequency strip (50), the default spectrum height (76) */
#define ON_Y    116
#define ON_H    76
#define OFF_H   116         /* off: from the top, under the bars and the strip */

/* LVGL's clock (lv_tick_mono.h): the GUI loop sets it, gui_main.c has it. Here it stays at 0: nothing waits on a timer */
uint32_t lv_tick_mono_now;

static lv_color_t buf[W * H];
static int        cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
    (void)a, (void)px;
    lv_disp_flush_ready(drv);
}

/* The object's LV_EVENT_DRAW_MAIN_END as lv_obj_redraw() sends it: into buf (the whole screen), clipped to clip */
static void draw(lv_obj_t *o, const lv_area_t *clip) {
    lv_area_t     buf_area = {0, 0, W - 1, H - 1};
    lv_draw_ctx_t ctx = {0};

    memset(buf, FILL, sizeof(buf));
    ctx.buf       = buf;
    ctx.buf_area  = &buf_area;
    ctx.clip_area = clip;
    lv_event_send(o, LV_EVENT_DRAW_MAIN_END, &ctx);
}

/* Every pixel in hole (NULL: none) all zero, every other one as filled */
static bool hole_is(const lv_area_t *hole) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t *p   = (const uint8_t *)&buf[y * W + x];
            bool           in  = hole && x >= hole->x1 && x <= hole->x2 && y >= hole->y1 && y <= hole->y2;
            uint8_t        want = in ? 0 : FILL;

            for (size_t i = 0; i < sizeof(lv_color_t); i++) {
                if (p[i] != want) {
                    printf("        pixel %d,%d byte %zu is 0x%02x, not 0x%02x\n", x, y, i, p[i], want);
                    return false;
                }
            }
        }
    }
    return true;
}

static void place(lv_obj_t *o, lv_coord_t y, lv_coord_t h) {
    lv_obj_set_y(o, y);
    lv_obj_set_height(o, h);
    lv_obj_update_layout(o);
}

int main(void) {
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t         px[W * 10];
    static lv_disp_drv_t      drv;
    const lv_area_t           screen = {0, 0, W - 1, H - 1};
    lv_area_t                 clip, hole;

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, px, NULL, W * 10);
    lv_disp_drv_init(&drv);
    drv.hor_res  = W;
    drv.ver_res  = H;
    drv.flush_cb = flush;
    drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&drv);

    /* spectrum_init() in direct mode, spectrum_layout() with the spectrum on */
    lv_obj_t *spectrum = lv_obj_create(lv_scr_act());

    lv_obj_set_x(spectrum, 0);
    lv_obj_set_width(spectrum, W);
    lv_obj_set_style_bg_opa(spectrum, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(spectrum, direct_hole_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    place(spectrum, ON_Y, ON_H);

    hole = (lv_area_t){0, ON_Y, W - 1, ON_Y + ON_H - 1};
    check(_lv_area_is_equal(&spectrum->coords, &hole), "laid out where spectrum_layout() puts it on: %d,%d to %d,%d",
          spectrum->coords.x1, spectrum->coords.y1, spectrum->coords.x2, spectrum->coords.y2);

    printf("Spectrum on\n");
    draw(spectrum, &screen);
    check(hole_is(&hole), "the whole screen redrawn: its area zeroed, nothing around it");

    clip = (lv_area_t){100, ON_Y - 16, 299, ON_Y + 13};
    draw(spectrum, &clip);
    check(hole_is(&(lv_area_t){100, ON_Y, 299, ON_Y + 13}), "a refresh across its top edge: only its part of the clip");

    clip = (lv_area_t){0, 0, W - 1, ON_Y - 1};
    draw(spectrum, &clip);
    check(hole_is(NULL), "a refresh of the top bars above it: nothing");

    printf("Spectrum off: transparent, under the top bars\n");
    lv_obj_set_style_opa(spectrum, LV_OPA_TRANSP, 0);
    place(spectrum, 0, OFF_H);
    draw(spectrum, &screen);
    check(hole_is(NULL), "the whole screen redrawn: no hole, the bars' gaps and corners keep the theme's background");

    clip = (lv_area_t){0, 0, 99, 65};
    draw(spectrum, &clip);
    check(hole_is(NULL), "a refresh of the top bar's left end: nothing");

    printf("Spectrum on again\n");
    lv_obj_remove_local_style_prop(spectrum, LV_STYLE_OPA, 0);
    place(spectrum, ON_Y, ON_H);
    draw(spectrum, &screen);
    check(hole_is(&hole), "its area zeroed again, nothing around it");

    printf("%d of %d cases ok\n", good, cases);
    return good == cases ? 0 : 1;
}
