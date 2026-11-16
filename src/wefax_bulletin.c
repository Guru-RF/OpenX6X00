/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax Bulletin: the kinds of chart as tiles, one of them large (wefax_bulletin.h).
 *
 *  The tiles draw from the charts' previews (wefax/thumb.h); a chart saved before them gets its preview from a
 *  worker thread the first time it is shown, its tile says "..." until then. The folder is read again every few
 *  seconds: Auto Plan's new charts come in by themselves.
 */

#include "wefax_bulletin.h"

#include "board.h"
#include "gui_styles.h"
#include "i18n.h"
#include "key_input.h"
#include "theme.h"
#include "wefax/decoder.h"
#include "wefax/plan.h"
#include "wefax/thumb.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SAVE_DIR    BOARD_DATA "/wefax"

#define COLS        4
#define ROWS        2
#define TILES       (COLS * ROWS)
#define TILE_W      186
#define PITCH_X     196
#define TEXT_H      44          /* the kind and its line under the picture */
#define SCAN_TICKS  5           /* the folder read again every this many seconds */
#define DOT         10

#define AGE_FRESH_S (6 * 3600)
#define AGE_OLD_S   (24 * 3600)

typedef struct {
    char     file[96];          /* its newest copy, "" none */
    uint8_t *prv;               /* that copy's preview, NULL not loaded */
    unsigned pw, ph;
    char     prv_of[96];        /* the file prv was loaded for */
    bool     prv_missing;       /* no preview on the card yet: the worker makes it */
} kind_t;

typedef struct {
    lv_obj_t   *box, *canvas, *empty, *name, *dot, *info;
    lv_color_t *buf;
} tile_t;

static wefax_bulletin_cbs_t cb;
static lv_obj_t   *root;
static lv_timer_t *timer;
static int         ticks;
static int         kinds_n, sel;
static bool        one;
static kind_t      kinds[WEFAX_BULLETIN_MAX];
static int         tile_h;              /* the picture's height in a tile */

static tile_t      tiles[TILES];
static lv_obj_t   *page_label;

/* the large view */
static lv_obj_t   *big, *big_canvas, *big_empty, *big_name, *big_src, *big_dot, *big_age, *big_taken, *big_mark,
                  *big_next, *big_hint, *big_dots[WEFAX_BULLETIN_MAX];
static lv_color_t *big_buf;
static int         big_w, big_h, big_x;   /* big_x: where the text starts */

/* previews made off the LVGL thread */
static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static bool            work_busy;
static char            work_files[WEFAX_BULLETIN_MAX][96];
static int             work_n;
static char            work_failed[WEFAX_BULLETIN_MAX][96];    /* no preview to be had: not tried again */
static int             work_failed_n;

static void key_cb(lv_event_t *e);
static void click_cb(lv_event_t *e);

/* ---- what the card has ---- */

static void *work_thread(void *arg)
{
    (void)arg;
    for (;;) {
        char name[96];

        pthread_mutex_lock(&work_lock);
        if (!work_n) {
            work_busy = false;
            pthread_mutex_unlock(&work_lock);
            return NULL;
        }
        snprintf(name, sizeof(name), "%s", work_files[--work_n]);
        pthread_mutex_unlock(&work_lock);
        if (wefax_preview_make(SAVE_DIR, name)) {
            printf("WEFAX: preview of %s made\n", name);
        } else {
            pthread_mutex_lock(&work_lock);
            snprintf(work_failed[work_failed_n % WEFAX_BULLETIN_MAX], sizeof(work_failed[0]), "%s", name);
            work_failed_n++;
            pthread_mutex_unlock(&work_lock);
        }
    }
}

static bool work_failed_has(const char *name)
{
    pthread_mutex_lock(&work_lock);
    int  n   = work_failed_n < WEFAX_BULLETIN_MAX ? work_failed_n : WEFAX_BULLETIN_MAX;
    bool has = false;
    for (int i = 0; i < n && !has; i++)
        has = !strcmp(work_failed[i], name);
    pthread_mutex_unlock(&work_lock);
    return has;
}

static void work_add(const char *name)
{
    if (work_failed_has(name))
        return;
    pthread_mutex_lock(&work_lock);
    for (int i = 0; i < work_n; i++)
        if (!strcmp(work_files[i], name)) {
            pthread_mutex_unlock(&work_lock);
            return;
        }
    if (work_n < WEFAX_BULLETIN_MAX)
        snprintf(work_files[work_n++], sizeof(work_files[0]), "%s", name);

    bool start = !work_busy;

    work_busy = true;
    pthread_mutex_unlock(&work_lock);

    pthread_t t;

    if (start) {
        if (pthread_create(&t, NULL, work_thread, NULL) == 0) {
            pthread_detach(t);
        } else {
            pthread_mutex_lock(&work_lock);
            work_busy = false;
            pthread_mutex_unlock(&work_lock);
        }
    }
}

/* The newest copy of each kind: the names sort by time */
static void scan(void)
{
    char newest[WEFAX_BULLETIN_MAX][96] = {{0}};
    DIR *dir = opendir(SAVE_DIR);

    if (dir) {
        struct dirent *de;

        while ((de = readdir(dir))) {
            const char *n   = de->d_name;
            size_t      len = strlen(n);

            char        chart[48];

            if (len >= sizeof(newest[0]) || strncmp(n, "wefax-", 6) || len < 4 || strcmp(n + len - 4, ".png"))
                continue;
            wefax_plan_name_product(n, chart, sizeof(chart));
            for (int k = 0; chart[0] && k < kinds_n; k++)
                if (wefax_bulletin_has(wefax_plan_bulletin(k), chart) && strcmp(n, newest[k]) > 0)
                    snprintf(newest[k], sizeof(newest[k]), "%s", n);
        }
        closedir(dir);
    }
    for (int k = 0; k < kinds_n; k++) {
        kind_t *c = &kinds[k];

        snprintf(c->file, sizeof(c->file), "%s", newest[k]);
        if (c->prv && strcmp(c->prv_of, c->file)) {
            free(c->prv);
            c->prv = NULL;
        }
        if (c->file[0] && !c->prv) {
            c->prv = wefax_preview_load(SAVE_DIR, c->file, &c->pw, &c->ph);
            snprintf(c->prv_of, sizeof(c->prv_of), "%s", c->file);
            c->prv_missing = !c->prv && !work_failed_has(c->file);   /* a failed one: not read every second */
            if (c->prv_missing)
                work_add(c->file);
        }
    }
}

static void age_text(char *out, size_t n, long s)
{
    if (s < 3600)
        snprintf(out, n, _("%ld min"), s < 60 ? 1 : s / 60);
    else if (s < 48 * 3600)
        snprintf(out, n, _("%ld h %02ld"), s / 3600, s % 3600 / 60);
    else
        snprintf(out, n, _("%ld days"), s / 86400);
}

static lv_color_t age_color(long s)
{
    return s < AGE_FRESH_S ? TC(ok) : s < AGE_OLD_S ? TC(warn) : TC(err);
}

/* When the plan has the kind next (UTC): "next 10:50, in 27 min", "" none */
static void next_text(char *out, size_t n, int k)
{
    int                 wait;
    const wefax_slot_t *s = wefax_plan_slot(wefax_plan_next_of_kind(wefax_plan_bulletin(k),
                                                                    wefax_plan_sod(time(NULL)), &wait));

    if (!s)
        out[0] = '\0';
    else if (wait < 60)
        snprintf(out, n, _("next %02d:%02d, now"), s->min / 60, s->min % 60);
    else if (wait < 3600)
        snprintf(out, n, _("next %02d:%02d, in %d min"), s->min / 60, s->min % 60, wait / 60);
    else
        snprintf(out, n, _("next %02d:%02d"), s->min / 60, s->min % 60);
}

/* ---- drawing ---- */

/* src (sw x sh grey) into dst (dw x dh), each pixel the mean of its box; crop: fill dst and cut the rest (the
 * middle kept), else the whole of src. Rows from `upto` on are dark (the chart coming in) */
static void blit(const uint8_t *src, unsigned sw, unsigned sh, unsigned upto, lv_color_t *dst, int dw, int dh,
                 bool crop)
{
    double sx = (double)sw / dw, sy = (double)sh / dh, s = crop ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
    double ox = (sw - dw * s) / 2, oy = (sh - dh * s) / 2;

    for (int y = 0; y < dh; y++) {
        int y0 = (int)(oy + y * s), y1 = (int)(oy + (y + 1) * s);

        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int      x0 = (int)(ox + x * s), x1 = (int)(ox + (x + 1) * s);
            uint32_t sum = 0, n = 0;

            if (x1 <= x0)
                x1 = x0 + 1;
            if (y0 >= 0 && y0 < (int)upto && x0 >= 0 && x0 < (int)sw)
                for (int yy = y0; yy < y1 && yy < (int)upto && yy < (int)sh; yy++)
                    for (int xx = x0; xx < x1 && xx < (int)sw; xx++) {
                        sum += src[(size_t)yy * sw + xx];
                        n++;
                    }
            dst[(size_t)y * dw + x] = n ? lv_color_make(sum / n, sum / n, sum / n) : TC(panel);
        }
    }
}

/* The chart coming in as a preview, its rows so far (malloc'ed, WEFAX_PREVIEW_W wide), NULL if not this kind */
static uint8_t *live_preview(int k, unsigned *pw, unsigned *ph, unsigned *upto, int *permille)
{
    wefax_bulletin_live_t lv = {0};

    if (cb.live)
        cb.live(&lv);
    if (!lv.product || !lv.row || !lv.rows || !wefax_bulletin_has(wefax_plan_bulletin(k), lv.product))
        return NULL;

    /* the chart's height as a saved one has it: about as tall as wide over 1.36 */
    unsigned w = WEFAX_PREVIEW_W, h = WEFAX_PREVIEW_H, full = WEFAX_WIDTH * WEFAX_PREVIEW_H / WEFAX_PREVIEW_W;
    uint8_t *img = malloc((size_t)w * h);

    if (!img)
        return NULL;
    if (lv.rows > full)
        full = lv.rows;
    *upto = (unsigned)((uint64_t)lv.rows * h / full);
    for (unsigned y = 0; y < *upto; y++) {
        const uint8_t *r = lv.row((unsigned)((uint64_t)y * full / h));

        for (unsigned x = 0; x < w; x++)
            img[(size_t)y * w + x] = r ? r[x * WEFAX_WIDTH / w] : 0;
    }
    *pw       = w;
    *ph       = h;
    *permille = lv.permille;
    return img;
}

static void tile_show(int t)
{
    tile_t *tl = &tiles[t];
    int     k  = (sel / TILES) * TILES + t;

    if (k >= kinds_n || !wefax_plan_bulletin(k) || !tl->buf) {
        lv_obj_add_flag(tl->box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(tl->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_opa(tl->box, k == sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_label_set_text(tl->name, wefax_plan_bulletin(k)->name);

    kind_t  *c = &kinds[k];
    unsigned pw, ph, upto;
    int      permille;
    uint8_t *live = live_preview(k, &pw, &ph, &upto, &permille);
    char     text[64], next[40];

    next_text(next, sizeof(next), k);
    if (live) {
        blit(live, pw, ph, upto, tl->buf, TILE_W, tile_h, true);
        free(live);
        lv_obj_add_flag(tl->empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tl->dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(tl->info, "receiving %d %%", permille / 10);
        lv_obj_set_style_text_color(tl->info, TC(text_head), LV_PART_MAIN);
        lv_obj_set_x(tl->info, 1);
        lv_obj_invalidate(tl->canvas);
        return;
    }
    lv_obj_set_style_text_color(tl->info, TC(text), LV_PART_MAIN);
    if (!c->file[0] || !c->prv) {
        lv_canvas_fill_bg(tl->canvas, TC(panel), LV_OPA_COVER);
        lv_obj_clear_flag(tl->empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(tl->empty, c->file[0] ? (c->prv_missing ? "..." : "no preview") : next[0] ? next : "not in the plan");
        lv_obj_add_flag(tl->dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(tl->info, 1);
        lv_label_set_text(tl->info, c->file[0] ? (c->prv_missing ? "preparing" : "a copy, unreadable") : "none yet");
        lv_obj_set_style_text_color(tl->info, TC(text_mute), LV_PART_MAIN);
        return;
    }

    long age = (long)(time(NULL) - wefax_plan_name_time(c->file));
    char a[16];
    int  q = wefax_plan_name_mark(c->file);

    blit(c->prv, c->pw, c->ph, c->ph, tl->buf, TILE_W, tile_h, true);
    lv_obj_invalidate(tl->canvas);
    lv_obj_add_flag(tl->empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tl->dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(tl->dot, age_color(age), LV_PART_MAIN);
    lv_obj_set_x(tl->info, 1 + DOT + 6);
    age_text(a, sizeof(a), age);
    if (q >= 0)
        snprintf(text, sizeof(text), _("%s ago, q%d"), a, q);
    else
        snprintf(text, sizeof(text), _("%s ago"), a);
    lv_label_set_text(tl->info, text);
}

static void big_show(void)
{
    if (sel >= kinds_n || !wefax_plan_bulletin(sel) || !big_buf)
        return;
    kind_t  *c = &kinds[sel];
    unsigned pw, ph, upto;
    int      permille;
    uint8_t *live = live_preview(sel, &pw, &ph, &upto, &permille);
    char     text[64], next[40];

    next_text(next, sizeof(next), sel);
    lv_label_set_text(big_name, wefax_plan_bulletin(sel)->name);
    lv_label_set_text(big_next, next);
    for (int i = 0; i < kinds_n; i++)
        lv_obj_set_style_bg_color(big_dots[i], i == sel ? TC(text_head) : TC(line), LV_PART_MAIN);

    const wefax_slot_t *s = c->file[0] ? wefax_plan_slot_of_file(c->file) : NULL;

    if (s)
        lv_label_set_text_fmt(big_src, "%s %.1f kHz", s->station, s->freq_hz / 1000.0);
    else
        lv_label_set_text(big_src, "");

    if (live) {
        blit(live, pw, ph, upto, big_buf, big_w, big_h, false);
        free(live);
        lv_obj_invalidate(big_canvas);
        lv_obj_add_flag(big_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(big_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(big_age, "receiving %d %%", permille / 10);
        lv_obj_set_style_text_color(big_age, TC(text_head), LV_PART_MAIN);
        lv_obj_set_x(big_age, big_x);
        lv_label_set_text(big_taken, c->file[0] ? "the last copy after it" : "");
        lv_label_set_text(big_mark, "");
        lv_label_set_text(big_hint, "MFK: kind");
        return;
    }
    lv_obj_set_style_text_color(big_age, TC(text), LV_PART_MAIN);
    if (!c->file[0] || !c->prv) {
        lv_canvas_fill_bg(big_canvas, TC(panel), LV_OPA_COVER);
        lv_obj_clear_flag(big_empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(big_empty, c->file[0] ? "preparing..." : "none yet");
        lv_obj_add_flag(big_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(big_age, big_x);
        lv_label_set_text(big_age, c->file[0] ? "" : "none yet");
        lv_label_set_text(big_taken, "");
        lv_label_set_text(big_mark, "");
        lv_label_set_text(big_hint, "MFK: kind");
        return;
    }

    time_t    t   = wefax_plan_name_time(c->file);
    long      age = (long)(time(NULL) - t);
    struct tm tm;
    char      a[16];
    int       q = wefax_plan_name_mark(c->file);

    blit(c->prv, c->pw, c->ph, c->ph, big_buf, big_w, big_h, false);
    lv_obj_invalidate(big_canvas);
    lv_obj_add_flag(big_empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(big_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(big_dot, age_color(age), LV_PART_MAIN);
    lv_obj_set_x(big_age, big_x + DOT + 14);
    age_text(a, sizeof(a), age);
    lv_label_set_text_fmt(big_age, "%s ago", a);
    gmtime_r(&t, &tm);
    snprintf(text, sizeof(text), _("taken %02d:%02d UTC, %02d/%02d"), tm.tm_hour, tm.tm_min, tm.tm_mday, tm.tm_mon + 1);
    lv_label_set_text(big_taken, text);
    if (q >= 0)
        lv_label_set_text_fmt(big_mark, "mark q%d", q);
    else
        lv_label_set_text(big_mark, "");
    lv_label_set_text(big_hint, "MFK: kind   press: full screen");
}

static void show(void)
{
    if (!root)
        return;
    int n = wefax_plan_bulletin_count();   /* Auto Plan may load the plan again meanwhile */
    if (n < kinds_n) {
        kinds_n = n;
        if (sel >= kinds_n)
            sel = kinds_n ? kinds_n - 1 : 0;
        if (!kinds_n)
            one = false;
    }
    if (one) {
        lv_obj_clear_flag(big, LV_OBJ_FLAG_HIDDEN);
        for (int t = 0; t < TILES; t++)
            lv_obj_add_flag(tiles[t].box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_label, LV_OBJ_FLAG_HIDDEN);
        big_show();
        return;
    }
    lv_obj_add_flag(big, LV_OBJ_FLAG_HIDDEN);
    for (int t = 0; t < TILES; t++)
        tile_show(t);
    if (kinds_n > TILES) {
        lv_obj_clear_flag(page_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(page_label, "%d/%d", sel / TILES + 1, (kinds_n + TILES - 1) / TILES);
    } else {
        lv_obj_add_flag(page_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (++ticks % SCAN_TICKS == 0 || kinds[sel].prv_missing)
        scan();
    show();
}

/* ---- objects ---- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *dot(lv_obj_t *parent, int x, int y, int d)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void tiles_create(int w, int h)
{
    int pitch_y = (h - 8) / ROWS;
    int x0      = (w - (COLS * PITCH_X - (PITCH_X - TILE_W))) / 2;

    tile_h = pitch_y - TEXT_H - 8;
    for (int t = 0; t < TILES; t++) {
        tile_t *tl = &tiles[t];

        tl->box = plain(root);
        lv_obj_set_size(tl->box, TILE_W + 8, pitch_y - 2);
        lv_obj_set_pos(tl->box, x0 + (t % COLS) * PITCH_X - 4, 4 + (t / COLS) * pitch_y);
        lv_obj_set_style_border_color(tl->box, TC(text_head), LV_PART_MAIN);
        lv_obj_set_style_border_width(tl->box, 3, LV_PART_MAIN);     /* shown on the one chosen only */
        lv_obj_set_style_radius(tl->box, 6, LV_PART_MAIN);

        tl->buf    = malloc(sizeof(lv_color_t) * TILE_W * tile_h);
        tl->canvas = lv_canvas_create(tl->box);
        if (tl->buf) {
            lv_canvas_set_buffer(tl->canvas, tl->buf, TILE_W, tile_h, LV_IMG_CF_TRUE_COLOR);
            lv_canvas_fill_bg(tl->canvas, TC(panel), LV_OPA_COVER);
        }
        lv_obj_set_pos(tl->canvas, 1, 1);

        tl->empty = lv_label_create(tl->box);
        lv_obj_set_style_text_font(tl->empty, &sony_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(tl->empty, TC(text_mute), LV_PART_MAIN);
        lv_obj_align(tl->empty, LV_ALIGN_TOP_MID, 0, 1 + tile_h / 2 - 12);

        tl->name = label(tl->box, &sony_18, TC(text), 1, tile_h + 5, TILE_W);
        tl->dot  = dot(tl->box, 1, tile_h + 5 + 25, DOT);
        tl->info = label(tl->box, &sony_16, TC(text), 1, tile_h + 5 + 21, TILE_W - DOT - 6);
    }
    page_label = label(root, &sony_16, TC(text_mute), w - 40, h - 22, 0);
}

static void big_create(int w, int h)
{
    big = plain(root);
    lv_obj_set_size(big, w, h);

    /* the whole chart, as tall as the window lets it */
    big_h = h - 12;
    if (big_h > WEFAX_PREVIEW_H)
        big_h = WEFAX_PREVIEW_H;
    big_w = big_h * WEFAX_PREVIEW_W / WEFAX_PREVIEW_H;
    big_buf    = malloc(sizeof(lv_color_t) * big_w * big_h);
    big_canvas = lv_canvas_create(big);
    if (big_buf) {
        lv_canvas_set_buffer(big_canvas, big_buf, big_w, big_h, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(big_canvas, TC(panel), LV_OPA_COVER);
    }
    lv_obj_set_pos(big_canvas, 8, (h - big_h) / 2);

    big_empty = lv_label_create(big);
    lv_obj_set_style_text_font(big_empty, &sony_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(big_empty, TC(text_mute), LV_PART_MAIN);
    lv_obj_align(big_empty, LV_ALIGN_LEFT_MID, 8 + big_w / 2 - 50, 0);

    int x = big_x = 8 + big_w + 20, tw = w - x - 8;

    big_name  = label(big, &sony_30, TC(text), x, 8, tw);
    big_src   = label(big, &sony_20, TC(text_mute), x, 46, tw);
    big_dot   = dot(big, x, 92, DOT + 4);
    big_age   = label(big, &sony_24, TC(text), x + DOT + 14, 84, tw - DOT - 14);
    big_taken = label(big, &sony_20, TC(text_mute), x, 120, tw);
    big_mark  = label(big, &sony_20, TC(text_mute), x, 148, tw);
    big_next  = label(big, &sony_20, TC(text_head), x, 184, tw);

    /* where the kind is among them, one dot each */
    for (int i = 0; i < kinds_n; i++)
        big_dots[i] = dot(big, x + i * 22, h - 54, 10);
    big_hint = label(big, &sony_16, TC(text_mute), x, h - 30, tw);
}

void wefax_bulletin_open(lv_obj_t *parent, const wefax_bulletin_cbs_t *cbs)
{
    if (root)
        return;
    cb      = *cbs;
    kinds_n = wefax_plan_bulletin_count();
    sel     = 0;
    one     = false;
    ticks   = 0;
    memset(kinds, 0, sizeof(kinds));

    lv_obj_update_layout(parent);

    int w = lv_obj_get_width(parent), h = lv_obj_get_height(parent);

    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, TC(frame), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(root, TC(dialog_border), LV_PART_MAIN);
    lv_obj_set_style_radius(root, 8, LV_PART_MAIN);

    tiles_create(w, h);
    big_create(w, h);

    if (!kinds_n) {
        lv_obj_t *l = label(root, &sony_24, TC(text_mute), 20, h / 2 - 14, w - 40);

        lv_label_set_text(l, "The plan has no Bulletin");
    }

    lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(root, click_cb, LV_EVENT_CLICKED, NULL);   /* the MFK's press */
    lv_group_add_obj(keyboard_group, root);
    lv_group_focus_obj(root);
    lv_group_set_editing(keyboard_group, true);

    scan();
    show();
    timer = lv_timer_create(timer_cb, 1000, NULL);
}

void wefax_bulletin_close(void)
{
    if (!root)
        return;
    lv_timer_del(timer);
    timer = NULL;
    lv_group_remove_obj(root);
    lv_obj_del(root);
    root = big = NULL;
    for (int t = 0; t < TILES; t++) {
        free(tiles[t].buf);
        memset(&tiles[t], 0, sizeof(tiles[t]));
    }
    free(big_buf);
    big_buf = NULL;
    for (int k = 0; k < WEFAX_BULLETIN_MAX; k++) {
        free(kinds[k].prv);
        kinds[k].prv = NULL;
    }
}

bool wefax_bulletin_is_open(void)
{
    return root != NULL;
}

bool wefax_bulletin_one(void)
{
    return root && one;
}

void wefax_bulletin_enter(void)
{
    if (!root || !kinds_n || sel >= wefax_plan_bulletin_count())
        return;
    if (!one) {
        one = true;
        show();
        if (cb.changed)
            cb.changed();
        return;
    }

    kind_t *c = &kinds[sel];

    if (c->file[0] && cb.view_file) {
        const wefax_slot_t *s = wefax_plan_slot_of_file(c->file);
        time_t              t = wefax_plan_name_time(c->file);
        struct tm           tm;
        char                name[96], title[96], src[40] = "";
        int                 q = wefax_plan_name_mark(c->file);

        gmtime_r(&t, &tm);
        if (s)
            snprintf(src, sizeof(src), "  %s %.1f kHz", s->station, s->freq_hz / 1000.0);
        snprintf(title, sizeof(title), "%s%s  %02d:%02d UTC", _(wefax_plan_bulletin(sel)->name), src, tm.tm_hour,
                 tm.tm_min);
        if (q >= 0)
            snprintf(title + strlen(title), sizeof(title) - strlen(title), "  q%d", q);
        snprintf(name, sizeof(name), "%s", c->file);
        cb.view_file(name, title);
    }
}

void wefax_bulletin_fix(void)
{
    if (root && one && kinds[sel].file[0] && cb.open_file) {
        char name[96];

        snprintf(name, sizeof(name), "%s", kinds[sel].file);
        cb.open_file(name);     /* closes the Bulletin */
    }
}

void wefax_bulletin_focus(void)
{
    if (!root)
        return;
    lv_group_add_obj(keyboard_group, root);
    lv_group_focus_obj(root);
    lv_group_set_editing(keyboard_group, true);
}

void wefax_bulletin_back(void)
{
    if (root && one) {
        one = false;
        show();
        if (cb.changed)
            cb.changed();
    }
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
            if (kinds_n) {
                sel = (sel + (key == LV_KEY_LEFT ? kinds_n - 1 : 1)) % kinds_n;
                show();
            }
            break;
        case LV_KEY_ENTER:
            wefax_bulletin_enter();
            break;
        case LV_KEY_ESC:
            wefax_bulletin_back();
            break;
        default:
            if (cb.other_keys)
                cb.other_keys(e);
            break;
    }
}

static void click_cb(lv_event_t *e)
{
    (void)e;
    wefax_bulletin_enter();
}
