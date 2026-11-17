/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax full screen viewer (wefax_viewer.h).
 *
 *  The chart is read whole (grey, 1809 px wide) and drawn into one screen-sized canvas at the zoom chosen: each
 *  screen pixel the mean of its box of chart pixels, so the thin lines stay at the smaller zooms. A step of the
 *  knobs moves an eighth of the screen. The knobs only mark it to be drawn again and a timer draws: a quick turn is
 *  one drawing, not one a step. Auto Tilt and Save run on threads of their own, each with its copy of the chart.
 */

#include "wefax_viewer.h"

#include "board.h"
#include "gui_events.h"
#include "gui_styles.h"
#include "i18n.h"
#include "key_input.h"
#include "screen_main.h"
#include "theme.h"
#include "wefax_night.h"
#include "wefax/thumb.h"
#include "wefax/tilt.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCR_W       BOARD_DISP_W
#define SCR_H       BOARD_DISP_H
#define MAP_W       96
#define TOP_H       28
#define DRAW_MS     40
#define ALIGN_STEP  8
#define GUIDE_PITCH 100         /* Fix: an upright line every this many px */

typedef enum { Z_WHOLE = 0, Z_TWO, Z_ONE, Z_N } zoom_t;

static lv_obj_t   *root, *canvas, *top, *bottom, *map, *map_box, *map_label;
static lv_timer_t *timer;
static lv_color_t *buf, *map_buf;
static uint8_t    *img;
static unsigned    img_w, img_h;
static zoom_t      zoom;
static double      cx, cy;          /* the chart pixel at the screen's middle */
static char        title[128], dir_of[192], name_of[96];
static void      (*closed)(void);
static bool        dirty, fixing, tilt_auto, saving;
static bool        tilt_pending;    /* a search asked while the last one (another chart's) still runs */
static unsigned    tilt_run_gen;    /* the chart the running search is for */
static int         align, tilt;
static unsigned    gen;             /* counts the charts opened: a worker's result is for one */

/* the workers' results */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool            tilt_busy, tilt_done, tilt_ok, save_done, save_ok;
static int             tilt_found;
static unsigned        tilt_gen, save_gen;

/* The height drawn: Fix leaves the buttons' row visible */
static int area_h(void)
{
    return fixing ? SCR_H - BUTTONS_H : SCR_H;
}

/* The small map's height: the chart's shape, a tall picture's cut to half the screen */
static int map_h(void)
{
    int mh = (int)((uint64_t)MAP_W * img_h / img_w);

    return mh < 1 ? 1 : mh > SCR_H / 2 ? SCR_H / 2 : mh;
}

static double scale(void)
{
    double s0 = (double)SCR_W / img_w < (double)area_h() / img_h ? (double)SCR_W / img_w : (double)area_h() / img_h;

    return zoom == Z_WHOLE ? s0 : zoom == Z_TWO ? 2 * s0 : 1.0;
}

/* The middle kept where the chart covers the screen, centred where it does not */
static void clamp(void)
{
    double s = scale(), hw = SCR_W / s / 2, hh = area_h() / s / 2;

    cx = img_w <= 2 * hw ? img_w / 2.0 : cx < hw ? hw : cx > img_w - hw ? img_w - hw : cx;
    cy = img_h <= 2 * hh ? img_h / 2.0 : cy < hh ? hh : cy > img_h - hh ? img_h - hh : cy;
}

/* Where row y's pixels come from in Fix: its Align and Tilt as the saved chart will have them (wefax_png_write_fixed) */
static int row_offset(int y)
{
    if (!fixing)
        return 0;

    int64_t sh  = ((int64_t)y - (int64_t)(img_h - 1)) * tilt * WEFAX_TILT_UNIT_W / (1000LL * img_w);
    int     off = (int)((-(int64_t)align - sh) % (int64_t)img_w);

    return off < 0 ? off + (int)img_w : off;
}

static void texts(void)
{
    static const char *hints[] = {N_("MFK press: zoom   F3: Fix   F5: back"),
                                  N_("MFK: up, down   VOL: left, right   press: zoom"),
                                  N_("MFK: up, down   VOL: left, right   press: whole chart")};

    if (saving) {
        lv_label_set_text(top, "Saving the chart...");
        return;
    }
    if (fixing) {
        lv_label_set_text_fmt(top, "#%06x Fix#   Tilt %+d%s   Align %+d      %s", theme_hex(TC_text_head), tilt,
                              tilt_busy ? _(" (searching)") : tilt_auto ? _(" (auto)") : "", align, title);
        lv_label_set_text(bottom, "MFK: tilt   VOL: align   press: auto tilt   the lines: the frame upright");
        return;
    }
    lv_label_set_text(top, title);
    lv_label_set_text(bottom, hints[zoom]);
}

static void draw(void)
{
    int        ah = area_h();
    double     s = scale(), x0 = cx - SCR_W / s / 2, y0 = cy - ah / s / 2;
    lv_color_t back = TC(frame), guide = TC(text_head);
    bool       night = wefax_night();

    for (int y = 0; y < ah; y++) {
        int sy0 = (int)(y0 + y / s), sy1 = (int)(y0 + (y + 1) / s);

        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < SCR_W; x++) {
            int      sx0 = (int)(x0 + x / s), sx1 = (int)(x0 + (x + 1) / s);
            uint32_t sum = 0, n = 0;

            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            if (sx0 >= 0 && sy0 >= 0 && sx0 < (int)img_w && sy0 < (int)img_h)
                for (int yy = sy0; yy < sy1 && yy < (int)img_h; yy++) {
                    const uint8_t *r   = img + (size_t)yy * img_w;
                    unsigned       off = (unsigned)row_offset(yy);

                    for (int xx = sx0; xx < sx1 && xx < (int)img_w; xx++) {
                        unsigned from = (unsigned)xx + off;

                        sum += r[from >= img_w ? from - img_w : from];
                        n++;
                    }
                }

            uint8_t    g = n ? wefax_night_grey((uint8_t)(sum / n), night) : 0;
            lv_color_t c = n ? lv_color_make(g, g, g) : back;

            if (fixing && n && x % GUIDE_PITCH == GUIDE_PITCH / 2)
                c = lv_color_mix(guide, c, 110);
            buf[(size_t)y * SCR_W + x] = c;
        }
    }
    lv_obj_invalidate(canvas);

    /* where the screen is, on the small map */
    int mh = map_h();
    int bx = (int)(x0 * MAP_W / img_w), by = (int)(y0 * mh / img_h);
    int bw = (int)(SCR_W / s * MAP_W / img_w), bh = (int)(ah / s * mh / img_h);

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

    lv_label_set_text(map_label, names[zoom]);
    texts();
}

/* Fix over the screen but the buttons; the hints beside the small map, else across */
static void layout(void)
{
    lv_obj_set_height(root, area_h());
    lv_obj_set_y(bottom, area_h() - TOP_H);
    lv_obj_set_width(bottom, fixing ? SCR_W : SCR_W - MAP_W - 22);
    if (fixing)
        lv_obj_add_flag(map, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(map, LV_OBJ_FLAG_HIDDEN);
}

static void step(double dx, double dy)
{
    double s = scale();

    cx += dx * SCR_W / s / 8;
    cy += dy * area_h() / s / 8;
    clamp();
    dirty = true;
}

/* ---- the workers ---- */

typedef struct {
    uint8_t *img;
    unsigned w, h, gen;
    int      align, tilt;
    char     path[300], dir[192], name[96];
} job_t;

/* A copy of the chart and its corrections for a worker */
static job_t *job_new(void)
{
    job_t *j = calloc(1, sizeof(*j));

    if (!j)
        return NULL;
    if (!(j->img = malloc((size_t)img_w * img_h))) {
        free(j);
        return NULL;
    }
    memcpy(j->img, img, (size_t)img_w * img_h);
    j->w     = img_w;
    j->h     = img_h;
    j->gen   = gen;
    j->align = align;
    j->tilt  = tilt;
    snprintf(j->dir, sizeof(j->dir), "%s", dir_of);
    snprintf(j->name, sizeof(j->name), "%s", name_of);
    snprintf(j->path, sizeof(j->path), "%s/%s", dir_of, name_of);
    return j;
}

static void job_free(job_t *j)
{
    free(j->img);
    free(j);
}

static void *tilt_worker(void *arg)
{
    job_t *j  = arg;
    int    t  = 0;
    bool   ok = wefax_auto_tilt(j->img, j->w, j->h, &t);

    pthread_mutex_lock(&lock);
    tilt_found = t;
    tilt_ok    = ok;
    tilt_gen   = j->gen;
    tilt_done  = true;
    tilt_busy  = false;
    pthread_mutex_unlock(&lock);
    job_free(j);
    return NULL;
}

/* The chart written over its own file, straightened, and its preview made again for the tiles */
static void *save_worker(void *arg)
{
    job_t *j  = arg;
    bool   ok = wefax_png_write_fixed(j->path, j->img, j->w, j->h, j->align, j->tilt);

    if (ok) {
        printf("WEFAX: saved %s, align %d tilt %d\n", j->path, j->align, j->tilt);
        wefax_preview_make(j->dir, j->name);
    }
    pthread_mutex_lock(&lock);
    save_ok   = ok;
    save_gen  = j->gen;
    save_done = true;
    pthread_mutex_unlock(&lock);
    job_free(j);
    return NULL;
}

static bool run(void *(*fn)(void *))
{
    job_t    *j = job_new();
    pthread_t th;

    if (!j)
        return false;
    if (pthread_create(&th, NULL, fn, j) != 0) {
        job_free(j);
        return false;
    }
    pthread_detach(th);
    return true;
}

/* The workers' results taken, and the drawing the knobs asked for */
static void timer_cb(lv_timer_t *t)
{
    (void)t;

    bool     td, tok, sd, sok;
    int      found;
    unsigned tg, sg;

    pthread_mutex_lock(&lock);
    td = tilt_done, tok = tilt_ok, found = tilt_found, tg = tilt_gen;
    sd = save_done, sok = save_ok, sg = save_gen;
    tilt_done = save_done = false;
    pthread_mutex_unlock(&lock);

    if (td && tg == gen && fixing) {
        if (tok) {
            tilt      = found;
            tilt_auto = true;
            dirty     = true;
        } else {
            channel_overlay_show("Auto Tilt: no clear tilt, MFK to set it");
            texts();
        }
    }
    if (tilt_pending && fixing) {
        pthread_mutex_lock(&lock);

        bool busy = tilt_busy;

        pthread_mutex_unlock(&lock);
        if (!busy) {
            tilt_pending = false;
            wefax_viewer_auto_tilt();
        }
    }
    if (sd && sg == gen && saving) {
        channel_overlay_show(sok ? "Chart saved" : "Saving failed");
        saving = false;
        wefax_viewer_close();
        return;
    }
    if (dirty) {
        dirty = false;
        draw();
    }
}

/* ---- keys ---- */

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (saving)
        return;
    if (fixing) {
        switch (key) {
            case LV_KEY_LEFT:
            case LV_KEY_RIGHT:
                tilt += key == LV_KEY_LEFT ? -1 : 1;
                tilt      = tilt < -WEFAX_TILT_MAX ? -WEFAX_TILT_MAX : tilt > WEFAX_TILT_MAX ? WEFAX_TILT_MAX : tilt;
                tilt_auto = false;
                dirty     = true;
                break;
            case KEY_VOL_LEFT_EDIT:
            case KEY_VOL_LEFT_SELECT:
            case KEY_VOL_RIGHT_EDIT:
            case KEY_VOL_RIGHT_SELECT:
                align += key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT ? -ALIGN_STEP : ALIGN_STEP;
                while (align < -(int)(img_w / 2))
                    align += (int)img_w;
                while (align > (int)(img_w / 2))
                    align -= (int)img_w;
                dirty = true;
                break;
            case LV_KEY_ENTER:
                wefax_viewer_auto_tilt();
                break;
            case LV_KEY_ESC:            /* nothing is written without Save */
                wefax_viewer_cancel();
                break;
            default:
                break;
        }
        return;
    }
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
            dirty = true;
            break;
        case LV_KEY_ESC:
            wefax_viewer_close();
            break;
        default:
            break;
    }
}

/* The MFK's press: the next zoom around the same middle; in Fix, the tilt searched */
static void click_cb(lv_event_t *e)
{
    (void)e;
    if (saving)
        return;
    if (fixing) {
        wefax_viewer_auto_tilt();
        return;
    }
    zoom = (zoom + 1) % Z_N;
    clamp();
    dirty = true;
}

/* A line of text over the chart, at y */
static lv_obj_t *strip(int y)
{
    lv_obj_t *l = lv_label_create(root);

    lv_obj_set_size(l, SCR_W, TOP_H);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_style_bg_color(l, TC(popup), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(l, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, TC(text), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &sony_18, LV_PART_MAIN);
    lv_obj_set_style_pad_left(l, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(l, 4, LV_PART_MAIN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, "");
    return l;
}

bool wefax_viewer_open(const char *dir, const char *name, const char *t, bool fix, void (*on_close)(void))
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
    snprintf(dir_of, sizeof(dir_of), "%s", dir);
    snprintf(name_of, sizeof(name_of), "%s", name);
    closed    = on_close;
    zoom      = Z_WHOLE;
    cx        = img_w / 2.0;
    cy        = img_h / 2.0;
    fixing       = false;
    saving       = false;
    tilt_auto    = false;
    tilt_pending = false;
    align = tilt = 0;
    gen++;

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

    top    = strip(0);
    bottom = strip(SCR_H - TOP_H);

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
    if (map_buf) {      /* each pixel the mean of its box: a chart's thin lines stay */
        bool night = wefax_night();

        lv_canvas_set_buffer(mc, map_buf, MAP_W, mh, LV_IMG_CF_TRUE_COLOR);
        for (int y = 0; y < mh; y++) {
            unsigned y0 = (unsigned)((uint64_t)y * img_h / mh), y1 = (unsigned)((uint64_t)(y + 1) * img_h / mh);

            for (int x = 0; x < MAP_W; x++) {
                unsigned x0 = (unsigned)((uint64_t)x * img_w / MAP_W), x1 = (unsigned)((uint64_t)(x + 1) * img_w / MAP_W);
                uint32_t sum = 0, n = 0;

                for (unsigned yy = y0; yy < (y1 > y0 ? y1 : y0 + 1) && yy < img_h; yy++)
                    for (unsigned xx = x0; xx < (x1 > x0 ? x1 : x0 + 1) && xx < img_w; xx++, n++)
                        sum += img[(size_t)yy * img_w + xx];

                uint8_t g = n ? wefax_night_grey((uint8_t)(sum / n), night) : 0;

                map_buf[(size_t)y * MAP_W + x] = lv_color_make(g, g, g);
            }
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

    timer = lv_timer_create(timer_cb, DRAW_MS, NULL);
    if (fix) {
        wefax_viewer_fix();
    } else {
        layout();
        clamp();
        draw();
    }
    return true;
}

void wefax_viewer_close(void)
{
    if (!root)
        return;
    lv_timer_del(timer);
    timer = NULL;
    lv_group_remove_obj(root);
    lv_obj_del(root);
    root   = NULL;
    fixing = saving = false;
    free(buf);
    free(map_buf);
    free(img);
    buf = map_buf = NULL;
    img = NULL;
    gen++;          /* a worker still at it: its result is for no one */
    if (closed)
        closed();
}

bool wefax_viewer_is_open(void)
{
    return root != NULL;
}

void wefax_viewer_fix(void)
{
    if (!root || fixing)
        return;
    fixing = true;
    zoom   = Z_WHOLE;
    layout();
    clamp();
    draw();
    if (!tilt)
        wefax_viewer_auto_tilt();   /* from the tilt that straightens it: the MFK only touches it up */
}

bool wefax_viewer_fixing(void)
{
    return root && fixing;
}

void wefax_viewer_auto_tilt(void)
{
    if (!root || !fixing || saving)
        return;
    pthread_mutex_lock(&lock);

    bool busy = tilt_busy;

    if (!busy)
        tilt_busy = true;
    pthread_mutex_unlock(&lock);
    if (busy) {
        if (tilt_run_gen != gen)
            tilt_pending = true;    /* its result would be for no one: this chart's search when it is done */
        return;
    }
    tilt_run_gen = gen;
    if (!run(tilt_worker)) {
        pthread_mutex_lock(&lock);
        tilt_busy = false;
        pthread_mutex_unlock(&lock);
    }
    texts();
}

void wefax_viewer_undo(void)
{
    if (!root || !fixing || saving)
        return;
    align = tilt = 0;
    tilt_auto    = false;
    dirty        = true;
}

void wefax_viewer_cancel(void)
{
    if (root && !saving)
        wefax_viewer_close();
}

void wefax_viewer_save(void)
{
    if (!root || saving)
        return;
    if (!fixing || (!align && !tilt)) {     /* nothing to write */
        wefax_viewer_close();
        return;
    }
    saving = true;
    texts();
    if (!run(save_worker)) {
        saving = false;
        channel_overlay_show("Saving failed");
        texts();
    }
}
