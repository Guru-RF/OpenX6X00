/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax full screen viewer (wefax_viewer.h).
 *
 *  The chart is read whole (grey, 1809 px wide) and drawn into one screen-sized canvas at the zoom chosen: each
 *  screen pixel the mean of its box of chart pixels, so the thin lines stay at the smaller zooms. A step of the
 *  knobs moves an eighth of the screen.
 */

#include "wefax_viewer.h"

#include "board.h"
#include "gui_events.h"
#include "gui_styles.h"
#include "i18n.h"
#include "key_input.h"
#include "theme.h"
#include "wefax/thumb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCR_W   BOARD_DISP_W
#define SCR_H   BOARD_DISP_H
#define MAP_W   96
#define TOP_H   26

typedef enum { Z_WHOLE = 0, Z_TWO, Z_ONE, Z_N } zoom_t;

static lv_obj_t   *root, *canvas, *top, *map, *map_box, *map_label;
static lv_color_t *buf, *map_buf;
static uint8_t    *img;
static unsigned    img_w, img_h;
static zoom_t      zoom;
static double      cx, cy;          /* the chart pixel at the screen's middle */
static char        title[96];
static void      (*closed)(void);

/* The small map's height: the chart's shape, a tall picture's cut to half the screen */
static int map_h(void)
{
    int mh = (int)((uint64_t)MAP_W * img_h / img_w);

    return mh < 1 ? 1 : mh > SCR_H / 2 ? SCR_H / 2 : mh;
}

static double scale(void)
{
    double s0 = (double)SCR_W / img_w < (double)SCR_H / img_h ? (double)SCR_W / img_w : (double)SCR_H / img_h;

    return zoom == Z_WHOLE ? s0 : zoom == Z_TWO ? 2 * s0 : 1.0;
}

/* The middle kept where the chart covers the screen, centred where it does not */
static void clamp(void)
{
    double s = scale(), hw = SCR_W / s / 2, hh = SCR_H / s / 2;

    cx = img_w <= 2 * hw ? img_w / 2.0 : cx < hw ? hw : cx > img_w - hw ? img_w - hw : cx;
    cy = img_h <= 2 * hh ? img_h / 2.0 : cy < hh ? hh : cy > img_h - hh ? img_h - hh : cy;
}

static void draw(void)
{
    double     s = scale(), x0 = cx - SCR_W / s / 2, y0 = cy - SCR_H / s / 2;
    lv_color_t back = TC(frame);

    for (int y = 0; y < SCR_H; y++) {
        int sy0 = (int)(y0 + y / s), sy1 = (int)(y0 + (y + 1) / s);

        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < SCR_W; x++) {
            int      sx0 = (int)(x0 + x / s), sx1 = (int)(x0 + (x + 1) / s);
            uint32_t sum = 0, n = 0;

            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            if (sx0 >= 0 && sy0 >= 0 && sx0 < (int)img_w && sy0 < (int)img_h)
                for (int yy = sy0; yy < sy1 && yy < (int)img_h; yy++)
                    for (int xx = sx0; xx < sx1 && xx < (int)img_w; xx++) {
                        sum += img[(size_t)yy * img_w + xx];
                        n++;
                    }
            buf[(size_t)y * SCR_W + x] = n ? lv_color_make(sum / n, sum / n, sum / n) : back;
        }
    }
    lv_obj_invalidate(canvas);

    /* where the screen is, on the small map */
    int mh = map_h();
    int bx = (int)(x0 * MAP_W / img_w), by = (int)(y0 * mh / img_h);
    int bw = (int)(SCR_W / s * MAP_W / img_w), bh = (int)(SCR_H / s * mh / img_h);

    if (bx < 0)
        bx = 0;
    if (by < 0)
        by = 0;
    if (bw > MAP_W)
        bw = MAP_W;
    if (bh > mh)
        bh = mh;
    lv_obj_set_pos(map_box, bx, by);
    lv_obj_set_size(map_box, bw, bh);

    static const char *names[] = {N_("whole"), "x2", "1:1"};
    static const char *hints[] = {N_("MFK press: zoom   any button: back"),
                                  N_("MFK: up, down   VOL: left, right   press: zoom"),
                                  N_("MFK: up, down   VOL: left, right   press: whole chart")};

    lv_label_set_text(map_label, names[zoom]);
    lv_label_set_text_fmt(top, "%s      %s", title, _(hints[zoom]));
}

static void step(double dx, double dy)
{
    double s = scale();

    cx += dx * SCR_W / s / 8;
    cy += dy * SCR_H / s / 8;
    clamp();
    draw();
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_LEFT:
            step(0, -1);
            break;
        case LV_KEY_RIGHT:
            step(0, 1);
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            step(-1, 0);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            step(1, 0);
            break;
        case LV_KEY_ENTER:
            zoom = (zoom + 1) % Z_N;
            clamp();
            draw();
            break;
        case LV_KEY_ESC:
            wefax_viewer_close();
            break;
        default:
            break;
    }
}

/* the MFK's press: the next zoom, around the same middle */
static void click_cb(lv_event_t *e)
{
    (void)e;
    zoom = (zoom + 1) % Z_N;
    clamp();
    draw();
}

bool wefax_viewer_open(const char *dir, const char *name, const char *t, void (*on_close)(void))
{
    char path[320];

    if (root)
        return false;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(img = wefax_png_read_grey(path, &img_w, &img_h)))
        return false;
    if (!(buf = malloc(sizeof(lv_color_t) * SCR_W * SCR_H))) {
        free(img);
        img = NULL;
        return false;
    }
    snprintf(title, sizeof(title), "%s", t ? t : name);
    closed = on_close;
    zoom   = Z_WHOLE;
    cx     = img_w / 2.0;
    cy     = img_h / 2.0;

    root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, SCR_W, SCR_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, TC(frame), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(canvas, buf, SCR_W, SCR_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(canvas, 0, 0);

    /* the top line, over the chart */
    top = lv_label_create(root);
    lv_obj_set_size(top, SCR_W, TOP_H);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, TC(popup), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_text_color(top, TC(text), LV_PART_MAIN);
    lv_obj_set_style_text_font(top, &sony_18, LV_PART_MAIN);
    lv_obj_set_style_pad_left(top, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(top, 3, LV_PART_MAIN);
    lv_label_set_long_mode(top, LV_LABEL_LONG_CLIP);

    /* the small map: the whole chart, a frame where the screen is */
    int mh = map_h();

    map = lv_obj_create(root);
    lv_obj_remove_style_all(map);
    lv_obj_set_size(map, MAP_W + 8, mh + 30);
    lv_obj_set_pos(map, SCR_W - MAP_W - 14, SCR_H - mh - 36);
    lv_obj_set_style_bg_color(map, TC(popup), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(map, LV_OPA_70, LV_PART_MAIN);
    lv_obj_clear_flag(map, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mc = lv_canvas_create(map);

    map_buf = malloc(sizeof(lv_color_t) * MAP_W * mh);
    if (map_buf) {
        lv_canvas_set_buffer(mc, map_buf, MAP_W, mh, LV_IMG_CF_TRUE_COLOR);
        for (int y = 0; y < mh; y++)
            for (int x = 0; x < MAP_W; x++) {
                uint8_t g = img[(size_t)(y * img_h / mh) * img_w + (size_t)x * img_w / MAP_W];

                map_buf[(size_t)y * MAP_W + x] = lv_color_make(g, g, g);
            }
    }
    lv_obj_set_pos(mc, 4, 4);

    map_box = lv_obj_create(mc);
    lv_obj_remove_style_all(map_box);
    lv_obj_set_style_border_color(map_box, TC(text_head), LV_PART_MAIN);
    lv_obj_set_style_border_width(map_box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(map_box, LV_OPA_COVER, LV_PART_MAIN);

    map_label = lv_label_create(map);
    lv_obj_set_style_text_color(map_label, TC(text_head), LV_PART_MAIN);
    lv_obj_set_style_text_font(map_label, &sony_16, LV_PART_MAIN);
    lv_obj_align(map_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(root, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(keyboard_group, root);
    lv_group_focus_obj(root);
    lv_group_set_editing(keyboard_group, true);

    clamp();
    draw();
    return true;
}

void wefax_viewer_close(void)
{
    if (!root)
        return;
    lv_group_remove_obj(root);
    lv_obj_del(root);
    root = NULL;
    free(buf);
    free(map_buf);
    free(img);
    buf = map_buf = NULL;
    img = NULL;
    if (closed)
        closed();
}

bool wefax_viewer_is_open(void)
{
    return root != NULL;
}
