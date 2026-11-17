/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax charts as tiles: the Bulletin (a kind of chart a tile) and All Charts (a chart a tile), one of
 *  them large (wefax_bulletin.h).
 *
 *  The tiles draw from the charts' previews (wefax/thumb.h), a few kept in memory for the page shown; a chart saved
 *  before them gets its preview from a worker thread the first time it is shown ("..." until then). The folder is
 *  read again every few seconds: Auto Plan's new charts come in by themselves.
 */

#include "wefax_bulletin.h"

#include "board.h"
#include "gui_styles.h"
#include "i18n.h"
#include "key_input.h"
#include "theme.h"
#include "wefax_night.h"
#include "wefax/decoder.h"
#include "wefax/plan.h"
#include "wefax/thumb.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAVE_DIR    BOARD_DATA "/wefax"

#define COLS        4
#define ROWS        2
#define TILES       (COLS * ROWS)
#define TILE_W      186
#define PITCH_X     196
#define TEXT_H      48          /* the name and its line under the picture */
#define SCAN_TICKS  5           /* the folder read again every this many seconds */
#define DOT         10
#define QUAL_W      50          /* the quality word on the right of a tile's line */
#define ITEMS_MAX   128         /* All Charts: the newest this many */
#define CACHE_N     12          /* previews kept: a page and a few */
#define COMING_N    4           /* the large view of a kind with no copy: its next broadcasts */
#define DELETE_MS   3000        /* Delete's second press within this */

#define AGE_FRESH_S (6 * 3600)
#define AGE_OLD_S   (24 * 3600)

typedef struct {
    char file[96];              /* "" none (a kind of the Bulletin with no copy yet) */
    int  kind;                  /* the Bulletin's kind it is, -1 none */
} item_t;

typedef struct {
    char     name[96];
    uint8_t *prv;
    unsigned w, h;
    uint32_t used;
} cache_t;

typedef struct {
    lv_obj_t   *box, *canvas, *empty, *name, *dot, *info, *qual;
    lv_color_t *buf;
} tile_t;

static wefax_bulletin_cbs_t cb;
static wefax_tiles_mode_t   mode;
static lv_obj_t   *root;
static lv_timer_t *timer;
static int         ticks;
static item_t      items[ITEMS_MAX];
static int         items_n, sel;
static char        sel_file[96];        /* All Charts: the tile chosen follows its chart when new ones come in */
static bool        one;
static int         tile_h;              /* the picture's height in a tile */
static cache_t     cache[CACHE_N];
static uint32_t    cache_clock;
static int64_t     delete_armed_ms;     /* 0: Delete not asked */

static tile_t      tiles[TILES];
static lv_obj_t   *page_label, *empty_label;

/* the large view */
static lv_obj_t   *big, *big_canvas, *big_empty, *big_name, *big_src, *big_dot, *big_age, *big_taken, *big_qual,
                  *big_next, *big_hint;
static lv_color_t *big_buf;
static int         big_w, big_h, big_x;   /* big_x: where the text starts */

/* the large view of a kind with no copy: its coming broadcasts */
static lv_obj_t   *coming, *coming_name, *coming_rows[COMING_N][4];

static lv_obj_t   *banner;              /* "Delete this chart?" */

/* previews made off the LVGL thread */
static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static bool            work_busy;
static char            work_files[TILES + 2][96];
static int             work_n;
static char            work_failed[16][96];     /* no preview to be had: not tried again */
static int             work_failed_n;

static void key_cb(lv_event_t *e);
static void click_cb(lv_event_t *e);

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- previews ---- */

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
            snprintf(work_failed[work_failed_n % 16], sizeof(work_failed[0]), "%s", name);
            work_failed_n++;
            pthread_mutex_unlock(&work_lock);
        }
    }
}

static bool work_failed_has(const char *name)
{
    pthread_mutex_lock(&work_lock);

    int  n   = work_failed_n < 16 ? work_failed_n : 16;
    bool has = false;

    for (int i = 0; i < n && !has; i++)
        has = !strcmp(work_failed[i], name);
    pthread_mutex_unlock(&work_lock);
    return has;
}

static void work_add(const char *name)
{
    pthread_mutex_lock(&work_lock);
    for (int i = 0; i < work_n; i++)
        if (!strcmp(work_files[i], name)) {
            pthread_mutex_unlock(&work_lock);
            return;
        }
    if (work_n < (int)(sizeof(work_files) / sizeof(work_files[0])))
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

typedef enum { PRV_OK, PRV_COMING, PRV_NONE } prv_state_t;

/* name's preview from the cache, else from the card (the least used one leaves), else asked of the worker */
static const cache_t *preview(const char *name, prv_state_t *st)
{
    cache_t *old = &cache[0];

    for (int i = 0; i < CACHE_N; i++) {
        if (cache[i].prv && !strcmp(cache[i].name, name)) {
            cache[i].used = ++cache_clock;
            *st           = PRV_OK;
            return &cache[i];
        }
        if (cache[i].used < old->used)
            old = &cache[i];
    }

    unsigned w, h;
    uint8_t *p = wefax_preview_load(SAVE_DIR, name, &w, &h);

    if (!p) {
        if (work_failed_has(name)) {
            *st = PRV_NONE;
        } else {
            *st = PRV_COMING;
            work_add(name);
        }
        return NULL;
    }
    free(old->prv);
    snprintf(old->name, sizeof(old->name), "%s", name);
    old->prv  = p;
    old->w    = w;
    old->h    = h;
    old->used = ++cache_clock;
    *st       = PRV_OK;
    return old;
}

static void cache_drop(const char *name)
{
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].prv && !strcmp(cache[i].name, name)) {
            free(cache[i].prv);
            memset(&cache[i], 0, sizeof(cache[i]));
        }
}

/* ---- the charts on the card ---- */

/* The first kind of the Bulletin a chart is, -1 none */
static int kind_of(const char *name)
{
    char chart[48];

    wefax_plan_name_product(name, chart, sizeof(chart));
    for (int k = 0; chart[0] && k < wefax_plan_bulletin_count(); k++)
        if (wefax_bulletin_has(wefax_plan_bulletin(k), chart))
            return k;
    return -1;
}

/* The Bulletin: each kind's copy (the newest of fair quality, else the newest). All Charts: the newest ITEMS_MAX of
 * however many there are, newest first, the one chosen kept */
static void scan(void)
{
    static char newest[ITEMS_MAX][96];
    int         n     = 0;
    int         kinds = wefax_plan_bulletin_count();
    DIR        *dir   = opendir(SAVE_DIR);

    if (mode == WEFAX_TILES_BULLETIN) {
        items_n = kinds < ITEMS_MAX ? kinds : ITEMS_MAX;
        for (int k = 0; k < items_n; k++) {
            items[k].file[0] = '\0';
            items[k].kind    = k;
        }
    }
    if (dir) {
        struct dirent *de;

        while ((de = readdir(dir))) {
            const char *f   = de->d_name;
            size_t      len = strlen(f);

            if (len >= sizeof(newest[0]) || len < 10 || strcmp(f + len - 4, ".png") ||
                wefax_plan_name_time(f) == (time_t)-1)
                continue;
            if (mode == WEFAX_TILES_BULLETIN) {
                char chart[48];

                wefax_plan_name_product(f, chart, sizeof(chart));
                for (int k = 0; chart[0] && k < items_n; k++)
                    if (wefax_bulletin_has(wefax_plan_bulletin(k), chart) &&
                        wefax_plan_bulletin_prefers(f, items[k].file))
                        snprintf(items[k].file, sizeof(items[k].file), "%.95s", f);   /* len checked above */
                continue;
            }

            /* in among the newest, newest first (the names sort by time) */
            int at = n;

            while (at > 0 && strcmp(f, newest[at - 1]) > 0)
                at--;
            if (at >= ITEMS_MAX)
                continue;
            if (n < ITEMS_MAX)
                n++;
            memmove(newest[at + 1], newest[at], (size_t)(n - 1 - at) * sizeof(newest[0]));
            snprintf(newest[at], sizeof(newest[0]), "%.95s", f);
        }
        closedir(dir);
    }
    if (mode == WEFAX_TILES_BULLETIN) {
        if (sel >= items_n)
            sel = items_n ? items_n - 1 : 0;
        return;
    }

    items_n = n;
    for (int i = 0; i < items_n; i++) {
        snprintf(items[i].file, sizeof(items[i].file), "%s", newest[i]);
        items[i].kind = kind_of(newest[i]);
    }

    int at = -1;

    for (int i = 0; i < items_n && sel_file[0]; i++)
        if (!strcmp(items[i].file, sel_file))
            at = i;
    sel = at >= 0 ? at : sel < items_n ? sel : items_n ? items_n - 1 : 0;
    if (items_n)
        snprintf(sel_file, sizeof(sel_file), "%s", items[sel].file);
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

/* The quality in a word (translated), and its colour */
static const char *quality(int q, lv_color_t *col)
{
    *col = q < 0 ? TC(text_mute) : q >= WEFAX_MARK_GOOD ? TC(ok) : q >= WEFAX_MARK_FAIR ? TC(warn) : TC(err);
    return q < 0 ? "" : _(wefax_plan_quality_word(q));
}

void wefax_bulletin_product_name(const char *product, char *out, size_t n)
{
    char        hours[16];
    const char *kind = wefax_plan_chart_kind(product, hours, sizeof(hours));

    if (!kind)
        snprintf(out, n, "%s", product);
    else if (hours[0])
        snprintf(out, n, "%s %s", _(kind), hours);
    else
        snprintf(out, n, "%s", _(kind));
}

/* An item named: the Bulletin's tile by its kind, All Charts' by the chart in words */
static void item_name(const item_t *it, char *out, size_t n)
{
    char chart[48];

    if (mode == WEFAX_TILES_BULLETIN && it->kind >= 0) {
        snprintf(out, n, "%s", _(wefax_plan_bulletin(it->kind)->name));
        return;
    }
    wefax_plan_name_product(it->file, chart, sizeof(chart));
    wefax_bulletin_product_name(chart, out, n);
}

/* "Northwood" for a chart of the plan, from its slot, else by its name's start; "" for one not of the plan */
static const char *station_of(const char *name)
{
    const wefax_slot_t *s = wefax_plan_slot_of_file(name);
    char                chart[48];

    if (s)
        return s->station;
    wefax_plan_name_product(name, chart, sizeof(chart));
    if (!strncmp(chart, "NW-", 3))
        return "Northwood";
    if (!strncmp(chart, "DWD-", 4))
        return "DWD";
    return "";
}

/* When a chart was received, UTC: "14:32" */
static void utc_time(const char *name, char *out, size_t n)
{
    time_t    t = wefax_plan_name_time(name);
    struct tm tm;

    gmtime_r(&t, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* The next broadcasts of what the item is (its kind, else its chart), soonest first */
static int coming_of(const item_t *it, int *slot, int *wait, int max)
{
    char chart[48];
    int  sod = wefax_plan_sod(time(NULL));

    if (it->kind >= 0 && mode == WEFAX_TILES_BULLETIN)
        return wefax_plan_coming(wefax_plan_bulletin(it->kind), NULL, sod, slot, wait, max);
    wefax_plan_name_product(it->file, chart, sizeof(chart));
    return chart[0] ? wefax_plan_coming(NULL, chart, sod, slot, wait, max) : 0;
}

static void wait_text(char *out, size_t n, int wait)
{
    if (wait < 60)
        snprintf(out, n, "%s", _("now"));
    else if (wait < 3600)
        snprintf(out, n, _("in %d min"), wait / 60);
    else
        snprintf(out, n, _("in %d h %02d"), wait / 3600, wait % 3600 / 60);
}

/* ---- drawing ---- */

/* src (sw x sh grey) into dst (dw x dh), each pixel the mean of its box; crop: fill dst and cut the rest (the
 * middle kept), else the whole of src. Rows from `upto` on are dark (the chart coming in). Dimmed at night */
static void blit(const uint8_t *src, unsigned sw, unsigned sh, unsigned upto, lv_color_t *dst, int dw, int dh,
                 bool crop)
{
    bool   night = wefax_night();
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

            uint8_t g = n ? wefax_night_grey((uint8_t)(sum / n), night) : 0;

            dst[(size_t)y * dw + x] = n ? lv_color_make(g, g, g) : TC(panel);
        }
    }
}

/* The chart coming in as a preview, its rows so far (malloc'ed, WEFAX_PREVIEW_W wide), NULL if not this item's */
static uint8_t *live_preview(const item_t *it, unsigned *pw, unsigned *ph, unsigned *upto, int *permille)
{
    wefax_bulletin_live_t lv = {0};

    if (mode != WEFAX_TILES_BULLETIN || it->kind < 0)
        return NULL;
    if (cb.live)
        cb.live(&lv);
    if (!lv.product || !lv.row || !lv.rows || !wefax_bulletin_has(wefax_plan_bulletin(it->kind), lv.product))
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

/* The tile's line under its name: where it starts, as wide as the quality on its right lets it, one line */
static void info_at(tile_t *tl, int x, bool qual)
{
    lv_obj_set_x(tl->info, x);
    lv_obj_set_width(tl->info, TILE_W - x - (qual ? QUAL_W : 2));
}

static void tile_show(int t)
{
    tile_t *tl = &tiles[t];
    int     k  = (sel / TILES) * TILES + t;

    if (k >= items_n || !tl->buf ||
        (mode == WEFAX_TILES_BULLETIN && (items[k].kind < 0 || !wefax_plan_bulletin(items[k].kind)))) {
        lv_obj_add_flag(tl->box, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    item_t *it = &items[k];
    char    name[48], text[64], when[32];

    lv_obj_clear_flag(tl->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_opa(tl->box, k == sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    item_name(it, name, sizeof(name));
    lv_label_set_text(tl->name, name);

    unsigned pw, ph, upto;
    int      permille;
    uint8_t *live = live_preview(it, &pw, &ph, &upto, &permille);

    lv_obj_set_style_text_color(tl->info, TC(text_soft), LV_PART_MAIN);
    if (live) {
        blit(live, pw, ph, upto, tl->buf, TILE_W, tile_h, true);
        free(live);
        lv_obj_add_flag(tl->empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tl->dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(tl->info, "receiving %d %%", permille / 10);
        lv_obj_set_style_text_color(tl->info, TC(text_head), LV_PART_MAIN);
        info_at(tl, 1, false);
        lv_label_set_text(tl->qual, "");
        lv_obj_invalidate(tl->canvas);
        return;
    }

    prv_state_t    st = PRV_NONE;
    const cache_t *c  = it->file[0] ? preview(it->file, &st) : NULL;

    if (!c) {
        int slot[1], wait[1];

        lv_canvas_fill_bg(tl->canvas, TC(panel), LV_OPA_COVER);
        lv_obj_clear_flag(tl->empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tl->dot, LV_OBJ_FLAG_HIDDEN);
        info_at(tl, 1, false);
        lv_label_set_text(tl->qual, "");
        if (it->file[0]) {
            lv_label_set_text(tl->empty, st == PRV_COMING ? "..." : "no preview");
            lv_label_set_text(tl->info, st == PRV_COMING ? "preparing" : "a copy, unreadable");
        } else if (coming_of(it, slot, wait, 1)) {
            const wefax_slot_t *s = wefax_plan_slot(slot[0]);

            wait_text(when, sizeof(when), wait[0]);
            lv_label_set_text_fmt(tl->empty, "next %02d:%02d", s->min / 60, s->min % 60);
            lv_label_set_text_fmt(tl->info, "none yet, %s", when);
        } else {
            lv_label_set_text(tl->empty, "not in the plan");
            lv_label_set_text(tl->info, "none yet");
        }
        return;
    }

    long       age = (long)(time(NULL) - wefax_plan_name_time(it->file));
    lv_color_t col;

    blit(c->prv, c->w, c->h, c->h, tl->buf, TILE_W, tile_h, true);
    lv_obj_invalidate(tl->canvas);
    lv_obj_add_flag(tl->empty, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(tl->qual, quality(wefax_plan_name_mark(it->file), &col));
    lv_obj_set_style_text_color(tl->qual, col, LV_PART_MAIN);
    if (mode == WEFAX_TILES_BULLETIN) {         /* how old: the dot and the time */
        lv_obj_clear_flag(tl->dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(tl->dot, age_color(age), LV_PART_MAIN);
        info_at(tl, 1 + DOT + 6, true);
        age_text(when, sizeof(when), age);
        snprintf(text, sizeof(text), _("%s ago"), when);
    } else {                                    /* when and who */
        lv_obj_add_flag(tl->dot, LV_OBJ_FLAG_HIDDEN);
        info_at(tl, 1, true);
        utc_time(it->file, when, sizeof(when));
        snprintf(text, sizeof(text), "%s %s", when, station_of(it->file));
    }
    lv_label_set_text(tl->info, text);
}

/* A kind with no copy yet: its coming broadcasts instead of a picture */
static void coming_show(const item_t *it)
{
    int  slot[COMING_N], wait[COMING_N];
    int  n = coming_of(it, slot, wait, COMING_N);
    char name[48], when[24], text[64];

    item_name(it, name, sizeof(name));
    lv_label_set_text(coming_name, name);
    for (int i = 0; i < COMING_N; i++) {
        for (int c = 0; c < 4; c++)
            if (i < n)
                lv_obj_clear_flag(coming_rows[i][c], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(coming_rows[i][c], LV_OBJ_FLAG_HIDDEN);
        if (i >= n)
            continue;

        const wefax_slot_t *s = wefax_plan_slot(slot[i]);

        lv_label_set_text_fmt(coming_rows[i][0], "%02d:%02d", s->min / 60, s->min % 60);
        lv_label_set_text_fmt(coming_rows[i][1], "%s %.1f kHz", s->station, s->freq_hz / 1000.0);
        wefax_bulletin_product_name(s->product, text, sizeof(text));
        lv_label_set_text(coming_rows[i][2], text);
        wait_text(when, sizeof(when), wait[i]);
        lv_label_set_text(coming_rows[i][3], when);
    }
}

static void big_show(void)
{
    if (sel >= items_n || !big_buf)
        return;

    item_t  *it = &items[sel];
    unsigned pw, ph, upto;
    int      permille, slot[1], wait[1];
    uint8_t *live = live_preview(it, &pw, &ph, &upto, &permille);
    char     name[48], text[64], when[24];

    if (!it->file[0] && !live) {                /* nothing yet: what is coming */
        lv_obj_add_flag(big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(coming, LV_OBJ_FLAG_HIDDEN);
        coming_show(it);
        return;
    }
    lv_obj_clear_flag(big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(coming, LV_OBJ_FLAG_HIDDEN);

    item_name(it, name, sizeof(name));
    lv_label_set_text(big_name, name);
    if (coming_of(it, slot, wait, 1)) {
        const wefax_slot_t *s = wefax_plan_slot(slot[0]);

        wait_text(when, sizeof(when), wait[0]);
        lv_label_set_text_fmt(big_next, "next copy %02d:%02d (%s), %s", s->min / 60, s->min % 60, s->station, when);
    } else {
        lv_label_set_text(big_next, "");
    }

    const wefax_slot_t *s = it->file[0] ? wefax_plan_slot_of_file(it->file) : NULL;

    if (s)
        lv_label_set_text_fmt(big_src, "%s  %.1f kHz", s->station, s->freq_hz / 1000.0);
    else
        lv_label_set_text(big_src, it->file[0] ? station_of(it->file) : "");

    /* where it is among them, and what the MFK does */
    snprintf(text, sizeof(text), "%d/%d", sel + 1, items_n);

    if (live) {
        blit(live, pw, ph, upto, big_buf, big_w, big_h, false);
        free(live);
        lv_obj_invalidate(big_canvas);
        lv_obj_add_flag(big_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(big_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(big_age, "receiving %d %%", permille / 10);
        lv_obj_set_style_text_color(big_age, TC(text_head), LV_PART_MAIN);
        lv_obj_set_x(big_age, big_x);
        lv_label_set_text(big_taken, it->file[0] ? "the last copy after it" : "");
        lv_label_set_text(big_qual, "");
        lv_label_set_text_fmt(big_hint, "%s   MFK: next", text);
        return;
    }
    lv_obj_set_style_text_color(big_age, TC(text), LV_PART_MAIN);

    prv_state_t    st;
    const cache_t *c   = preview(it->file, &st);
    time_t         t   = wefax_plan_name_time(it->file);
    long           age = (long)(time(NULL) - t);
    int            q   = wefax_plan_name_mark(it->file);
    struct tm      tm;
    lv_color_t     col;

    if (c) {
        blit(c->prv, c->w, c->h, c->h, big_buf, big_w, big_h, false);
        lv_obj_add_flag(big_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_canvas_fill_bg(big_canvas, TC(panel), LV_OPA_COVER);
        lv_obj_clear_flag(big_empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(big_empty, st == PRV_COMING ? "preparing..." : "no preview");
    }
    lv_obj_invalidate(big_canvas);
    lv_obj_clear_flag(big_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(big_dot, age_color(age), LV_PART_MAIN);
    lv_obj_set_x(big_age, big_x + DOT + 14);
    age_text(when, sizeof(when), age);
    lv_label_set_text_fmt(big_age, "%s ago", when);
    gmtime_r(&t, &tm);
    lv_label_set_text_fmt(big_taken, "received %02d:%02d UTC, %02d/%02d", tm.tm_hour, tm.tm_min, tm.tm_mday,
                          tm.tm_mon + 1);
    if (q >= 0) {
        const char *w = quality(q, &col);

        lv_label_set_text_fmt(big_qual, "quality: %s (%d)", w, q);
        lv_obj_set_style_text_color(big_qual, col, LV_PART_MAIN);
    } else {
        lv_label_set_text(big_qual, "");
    }
    lv_label_set_text_fmt(big_hint, "%s   MFK: next   press: full screen", text);
}

static void banner_show(void)
{
    if (wefax_bulletin_delete_armed())
        lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(banner);
}

static void show(void)
{
    if (!root)
        return;
    if (mode == WEFAX_TILES_BULLETIN && items_n > wefax_plan_bulletin_count()) {   /* Auto Plan loaded the plan again */
        items_n = wefax_plan_bulletin_count();
        if (sel >= items_n)
            sel = items_n ? items_n - 1 : 0;
    }
    if (!items_n)
        one = false;
    if (items_n)
        lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
    if (one) {
        for (int t = 0; t < TILES; t++)
            lv_obj_add_flag(tiles[t].box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_label, LV_OBJ_FLAG_HIDDEN);
        big_show();
        banner_show();
        return;
    }
    lv_obj_add_flag(big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(coming, LV_OBJ_FLAG_HIDDEN);
    for (int t = 0; t < TILES; t++)
        tile_show(t);
    if (items_n > TILES) {
        lv_obj_clear_flag(page_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(page_label, "%d/%d", sel / TILES + 1, (items_n + TILES - 1) / TILES);
        lv_obj_move_foreground(page_label);
    } else {
        lv_obj_add_flag(page_label, LV_OBJ_FLAG_HIDDEN);
    }
    banner_show();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (delete_armed_ms && now_ms() - delete_armed_ms > DELETE_MS) {
        delete_armed_ms = 0;            /* not asked again in time: Delete as it was */
        if (cb.changed)
            cb.changed();
    }
    if (++ticks % SCAN_TICKS == 0)
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
        lv_obj_set_style_text_color(tl->empty, TC(text_soft), LV_PART_MAIN);
        lv_obj_align(tl->empty, LV_ALIGN_TOP_MID, 0, 1 + tile_h / 2 - 12);

        tl->name = label(tl->box, &sony_20, TC(text), 1, tile_h + 3, TILE_W);
        tl->dot  = dot(tl->box, 1, tile_h + 3 + 30, DOT);
        tl->info = label(tl->box, &sony_18, TC(text_soft), 1, tile_h + 3 + 24, TILE_W - DOT - QUAL_W);
        lv_obj_set_height(tl->info, 22);        /* one line, the rest dotted */
        tl->qual = lv_label_create(tl->box);
        lv_obj_set_style_text_font(tl->qual, &sony_18, LV_PART_MAIN);
        lv_label_set_text(tl->qual, "");
        lv_obj_align(tl->qual, LV_ALIGN_BOTTOM_RIGHT, -4, -6);
    }

    /* which page of them, over the top right tile's corner */
    page_label = label(root, &sony_18, TC(text_soft), 0, 0, 0);
    lv_obj_set_style_bg_color(page_label, TC(popup), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(page_label, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(page_label, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(page_label, 1, LV_PART_MAIN);
    lv_obj_align(page_label, LV_ALIGN_TOP_RIGHT, -8, 6);
}

static void big_create(int w, int h)
{
    big = plain(root);
    lv_obj_set_size(big, w, h);

    /* the whole chart, as tall as the window lets it */
    big_h = h - 12;
    if (big_h > WEFAX_PREVIEW_H)
        big_h = WEFAX_PREVIEW_H;
    big_w   = big_h * WEFAX_PREVIEW_W / WEFAX_PREVIEW_H;
    big_buf = malloc(sizeof(lv_color_t) * big_w * big_h);
    big_canvas = lv_canvas_create(big);
    if (big_buf) {
        lv_canvas_set_buffer(big_canvas, big_buf, big_w, big_h, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(big_canvas, TC(panel), LV_OPA_COVER);
    }
    lv_obj_set_pos(big_canvas, 8, (h - big_h) / 2);

    big_empty = lv_label_create(big);
    lv_obj_set_style_text_font(big_empty, &sony_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(big_empty, TC(text_soft), LV_PART_MAIN);
    lv_obj_align(big_empty, LV_ALIGN_LEFT_MID, 8 + big_w / 2 - 60, 0);

    int x = big_x = 8 + big_w + 20, tw = w - x - 8;

    big_name  = label(big, &sony_30, TC(text), x, 6, tw);
    big_src   = label(big, &sony_20, TC(text_soft), x, 44, tw);
    big_dot   = dot(big, x, 88, DOT + 4);
    big_age   = label(big, &sony_24, TC(text), x + DOT + 14, 80, tw - DOT - 14);
    big_taken = label(big, &sony_20, TC(text_soft), x, 114, tw);
    big_qual  = label(big, &sony_20, TC(ok), x, 142, tw);
    big_next  = label(big, &sony_20, TC(text_head), x, 170, tw);

    /* what the age dots say, under the details */
    lv_obj_t *legend = plain(big);

    lv_obj_set_size(legend, tw, 22);
    lv_obj_set_pos(legend, x, h - 56);

    static const char *ages[] = {"< 6 h", "< 24 h", N_("older")};

    for (int i = 0; i < 3; i++) {
        lv_obj_t *d = dot(legend, i * 104, 6, DOT);

        lv_obj_set_style_bg_color(d, i == 0 ? TC(ok) : i == 1 ? TC(warn) : TC(err), LV_PART_MAIN);
        lv_label_set_text(label(legend, &sony_16, TC(text_soft), i * 104 + 16, 0, 0), _(ages[i]));
    }
    big_hint = label(big, &sony_18, TC(text_soft), x, h - 30, tw);
}

static void coming_create(int w, int h)
{
    coming = plain(root);
    lv_obj_set_size(coming, w, h);
    coming_name = label(coming, &sony_30, TC(text), 16, 6, w - 32);
    lv_label_set_text(label(coming, &sony_20, TC(text_soft), 16, 44, w - 32), "none received yet, coming (UTC):");

    int rh = (h - 110) / COMING_N;

    for (int i = 0; i < COMING_N; i++) {
        lv_obj_t *row = plain(coming);

        lv_obj_set_size(row, w - 24, rh - 6);
        lv_obj_set_pos(row, 12, 74 + i * rh);
        lv_obj_set_style_bg_color(row, TC(row), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        coming_rows[i][0] = label(row, &sony_22, TC(text_head), 12, (rh - 6) / 2 - 12, 0);
        coming_rows[i][1] = label(row, &sony_20, TC(text), 92, (rh - 6) / 2 - 11, 260);
        coming_rows[i][2] = label(row, &sony_20, TC(text_soft), 362, (rh - 6) / 2 - 11, 250);
        coming_rows[i][3] = lv_label_create(row);
        lv_obj_set_style_text_font(coming_rows[i][3], &sony_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(coming_rows[i][3], TC(text_soft), LV_PART_MAIN);
        lv_obj_align(coming_rows[i][3], LV_ALIGN_RIGHT_MID, -12, 0);
    }
    lv_label_set_text(label(coming, &sony_18, TC(text_soft), 16, h - 28, w - 32),
                      "Auto Plan takes them by itself   MFK: next");
    lv_obj_add_flag(coming, LV_OBJ_FLAG_HIDDEN);
}

static void banner_create(int w, int h)
{
    banner = plain(root);
    lv_obj_set_size(banner, 420, 58);
    lv_obj_set_pos(banner, w - 430, h - 66);
    lv_obj_set_style_bg_color(banner, TC(err_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(banner, TC(err), LV_PART_MAIN);
    lv_obj_set_style_border_width(banner, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(banner, 8, LV_PART_MAIN);
    lv_label_set_text(label(banner, &sony_20, TC(text), 12, 4, 0), "Delete this chart?");
    lv_label_set_text(label(banner, &sony_18, TC(text), 12, 30, 0), "press Delete again (3 s)");
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

void wefax_bulletin_open(lv_obj_t *parent, wefax_tiles_mode_t m, const wefax_bulletin_cbs_t *cbs)
{
    if (root)
        return;
    cb              = *cbs;
    mode            = m;
    sel             = 0;
    sel_file[0]     = '\0';
    one             = false;
    ticks           = 0;
    items_n         = 0;
    delete_armed_ms = 0;

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
    coming_create(w, h);
    banner_create(w, h);
    empty_label = label(root, &sony_24, TC(text_soft), 20, h / 2 - 14, w - 40);
    lv_label_set_text(empty_label, m == WEFAX_TILES_BULLETIN ? "The plan has no Bulletin" : "No chart saved yet");
    lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);

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
    root = big = coming = banner = empty_label = page_label = NULL;
    for (int t = 0; t < TILES; t++) {
        free(tiles[t].buf);
        memset(&tiles[t], 0, sizeof(tiles[t]));
    }
    free(big_buf);
    big_buf = NULL;
    for (int i = 0; i < CACHE_N; i++) {
        free(cache[i].prv);
        memset(&cache[i], 0, sizeof(cache[i]));
    }
}

bool wefax_bulletin_is_open(void)
{
    return root != NULL;
}

wefax_tiles_mode_t wefax_bulletin_mode(void)
{
    return mode;
}

bool wefax_bulletin_one(void)
{
    return root && one;
}

bool wefax_bulletin_has_chart(void)
{
    return root && items_n && sel < items_n && items[sel].file[0];
}

static void disarm(void)
{
    if (delete_armed_ms) {
        delete_armed_ms = 0;
        if (cb.changed)
            cb.changed();
    }
}

static void choose(int k)
{
    if (!items_n)
        return;
    sel = (k % items_n + items_n) % items_n;
    if (mode == WEFAX_TILES_ALL)
        snprintf(sel_file, sizeof(sel_file), "%s", items[sel].file);
    disarm();
    show();
    if (cb.changed)
        cb.changed();               /* a kind with no chart yet: nothing to show full screen, fix or delete */
}

/* The one chosen over the whole screen; its title what it is, from where, when and how good */
static void view(bool fix)
{
    item_t *it = &items[sel];

    if (!it->file[0] || !cb.view_file)
        return;

    const wefax_slot_t *s = wefax_plan_slot_of_file(it->file);
    char                name[96], title[128], what[48], src[40] = "", when[8];
    int                 q = wefax_plan_name_mark(it->file);
    lv_color_t          col;

    item_name(it, what, sizeof(what));
    utc_time(it->file, when, sizeof(when));
    if (s)
        snprintf(src, sizeof(src), "  %s %.1f kHz", s->station, s->freq_hz / 1000.0);
    snprintf(title, sizeof(title), "%s%s  %s UTC", what, src, when);
    if (q >= 0)
        snprintf(title + strlen(title), sizeof(title) - strlen(title), "  %s", quality(q, &col));
    snprintf(name, sizeof(name), "%s", it->file);
    disarm();
    cb.view_file(name, title, fix);
}

void wefax_bulletin_enter(void)
{
    if (!root || !items_n || sel >= items_n)
        return;
    if (!one) {
        one = true;
        disarm();
        show();
        if (cb.changed)
            cb.changed();
        return;
    }
    view(false);
}

void wefax_bulletin_fix(void)
{
    if (root && items_n && sel < items_n)
        view(true);
}

bool wefax_bulletin_back(void)
{
    if (!root || !one)
        return false;
    one = false;
    disarm();
    show();
    if (cb.changed)
        cb.changed();
    return true;
}

void wefax_bulletin_newest(void)
{
    if (root)
        choose(0);
}

bool wefax_bulletin_delete_armed(void)
{
    return delete_armed_ms && now_ms() - delete_armed_ms <= DELETE_MS;
}

void wefax_bulletin_delete(void)
{
    if (!root || !items_n || sel >= items_n || !items[sel].file[0])
        return;
    if (!wefax_bulletin_delete_armed()) {       /* the first press asks */
        delete_armed_ms = now_ms();
        show();
        if (cb.changed)
            cb.changed();
        return;
    }

    char path[160], name[96];

    snprintf(name, sizeof(name), "%s", items[sel].file);
    snprintf(path, sizeof(path), SAVE_DIR "/%s", name);
    delete_armed_ms = 0;
    if (unlink(path) == 0) {
        wefax_preview_remove(SAVE_DIR, name);
        cache_drop(name);
        printf("WEFAX: deleted %s\n", path);
    } else {
        printf("WEFAX: can't delete %s\n", path);
    }
    if (mode == WEFAX_TILES_ALL) {
        sel_file[0] = '\0';                     /* the one after it takes its place */
        if (one && items_n <= 1)
            one = false;
    }
    scan();
    if (mode == WEFAX_TILES_ALL && items_n)
        snprintf(sel_file, sizeof(sel_file), "%s", items[sel < items_n ? sel : items_n - 1].file);
    show();
    if (cb.changed)
        cb.changed();
}

void wefax_bulletin_focus(void)
{
    if (!root)
        return;
    lv_group_add_obj(keyboard_group, root);
    lv_group_focus_obj(root);
    lv_group_set_editing(keyboard_group, true);
    if (items_n && sel < items_n)
        cache_drop(items[sel].file);            /* fixed and saved there: its new preview */
    scan();
    show();
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
            choose(sel + (key == LV_KEY_LEFT ? -1 : 1));
            break;
        case LV_KEY_ENTER:
            wefax_bulletin_enter();
            break;
        case LV_KEY_ESC:                        /* as F5: one level up */
            if (!wefax_bulletin_back() && cb.back)
                cb.back();
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
