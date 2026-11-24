/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Load meter: see load_meter.h
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "load_meter.h"

#include "cfg/cfg_api.h"
#include "gui_styles.h"
#include "load_stat.h"
#include "screen_light.h"
#include "theme.h"

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PERIOD_MS 1000

/* No edge of the screen is free in every app. The left one: there Settings has only its tabs' icons (their names
 * start further in), and the apps the first letters of some lines; the right one has Settings' switches and values.
 * Under the band plan strip of every theme (VISION's is the lowest, to y 220) and above the knob lines at the bottom
 * left (VISION's are the highest, from y 345). On a panel of some 230 pixels an inch a bar 7 px wide is 0.8 mm */
#define X       0
#define Y       224
#define PAD     2
#define BAR_W   7
#define GAP     2
#define TEXT_H  14      /* pixel_14: its capitals are 6 px wide, 10 high */
#define BAR_H   82
#define DOT     8
#define BOX_H   (PAD + TEXT_H + 3 + BAR_H + 5 + DOT + PAD)

LV_FONT_DECLARE(pixel_14);     /* not one the theme's font choice swaps: the letters keep their size in VISION too */

enum { BAR_CPU, BAR_GUI, BAR_MEM, BAR_SWAP, BARS };

static const char *const letter[BARS] = {"C", "G", "M", "S"};

/* What the bars show: compared with what was drawn, so a second that changes no pixel draws nothing */
typedef struct {
    int8_t  px[BARS];       /* the fill, pixels; -1 no such bar (the swap without swap) */
    int8_t  peak[BARS];     /* the 10 s highest, pixels */
    uint8_t level[BARS];    /* load_level_t: the fill's colour */
    uint8_t late;           /* load_level_t of the dot */
} meter_state_t;

static lv_obj_t     *meter;
static lv_timer_t   *timer;
static char          task_path[64];     /* the GUI thread's stat */
static load_cpu_t    cpu_was;
static uint64_t      task_was;
static bool          have_was;
static int           cpu_pct, gui_pct, mem_pct;    /* the last that could be worked out */
static load_hold_t   hold_cpu, hold_gui, hold_late;
static uint32_t      busy_max;          /* this second's longest time the keys could not be read, ms */
static meter_state_t cur, drawn;

void load_meter_busy(uint32_t ms) {
    if (timer && ms > busy_max) busy_max = ms;
}

static int bars_shown(void) {
    return cur.px[BAR_SWAP] >= 0 ? BARS : BARS - 1;
}

static lv_color_t fill_color(int bar, load_level_t level) {
    if (level == LOAD_LEVEL_HIGH) return TC(bar_high);
    if (level == LOAD_LEVEL_MID) return TC(bar_mid);
    return bar >= BAR_MEM ? TC(info) : TC(ok_bar);
}

static void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t       *ctx = lv_event_get_draw_ctx(e);
    lv_obj_t            *obj = lv_event_get_target(e);
    lv_draw_rect_dsc_t   rect;
    lv_draw_label_dsc_t  text;
    lv_area_t            a   = obj->coords;
    lv_coord_t           top = a.y1 + PAD + TEXT_H + 3, bottom = top + BAR_H - 1;
    int                  n   = bars_shown();

    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = TC(panel);  /* readable over the waterfall, a little of it seen through */
    rect.bg_opa   = LV_OPA_60;
    rect.radius   = 3;
    lv_draw_rect(ctx, &rect, &a);
    rect.radius = 0;
    rect.bg_opa = LV_OPA_COVER;

    lv_draw_label_dsc_init(&text);
    text.font  = &pixel_14;
    text.color = TC(text_dim);
    text.align = LV_TEXT_ALIGN_CENTER;

    for (int i = 0; i < n; i++) {
        lv_area_t b = {a.x1 + PAD + i * (BAR_W + GAP), a.y1 + PAD, 0, 0};

        b.x2 = b.x1 + BAR_W - 1;
        b.y2 = b.y1 + TEXT_H - 1;
        lv_draw_label(ctx, &text, &b, letter[i], NULL);

        b.y1 = top;
        b.y2 = bottom;
        rect.bg_color     = TC(track);
        rect.border_width = theme_shape.mono ? 1 : 0;  /* 1bit: the track is the paper; hollow, the scale stays seen */
        rect.border_color = TC(text);
        lv_draw_rect(ctx, &rect, &b);
        rect.border_width = 0;
        if (cur.px[i] > 0) {
            b.y1          = bottom - cur.px[i] + 1;
            rect.bg_color = fill_color(i, (load_level_t)cur.level[i]);
            lv_draw_rect(ctx, &rect, &b);
        }
        if (cur.peak[i] > cur.px[i]) {     /* inside the fill it says nothing */
            b.y1          = bottom - cur.peak[i] + 1;
            b.y2          = b.y1 + 1;
            rect.bg_color = TC(text);
            lv_draw_rect(ctx, &rect, &b);
        }
    }

    /* The late loop's dot, under the middle of the bars; the track's colour while nothing was late, so its place is
     * known */
    lv_coord_t mid = a.x1 + (lv_area_get_width(&a) - DOT) / 2;
    lv_area_t  dot = {mid, bottom + 6, mid + DOT - 1, bottom + 5 + DOT};

    rect.radius       = LV_RADIUS_CIRCLE;
    rect.bg_color     = cur.late == LOAD_LEVEL_HIGH ? TC(err) : cur.late == LOAD_LEVEL_MID ? TC(warn) : TC(track);
    rect.border_width = theme_shape.mono && cur.late == LOAD_LEVEL_OK ? 1 : 0;
    lv_draw_rect(ctx, &rect, &dot);
}

static void bar_set(int bar, int pct, load_hold_t *hold) {
    cur.px[bar]    = (int8_t)load_px(pct, BAR_H);
    cur.peak[bar]  = hold ? (int8_t)load_px(load_hold_push(hold, pct), BAR_H) : -1;
    cur.level[bar] = (uint8_t)load_level(pct);
}

/* Once a second: the three files read, the bars worked out, drawn again only when they changed and are seen */
static void sample_cb(lv_timer_t *t) {
    char       buf[2048];      /* /proc/stat's cpu lines come first; the rest (the interrupts) is not needed */
    load_cpu_t cpu;
    load_mem_t mem;
    uint64_t   task = 0;
    uint32_t   busy = busy_max;
    bool       cpu_ok, task_ok;

    (void)t;
    busy_max = 0;
    cpu_ok   = load_read("/proc/stat", buf, sizeof(buf)) && load_cpu_parse(buf, &cpu);
    task_ok  = load_read(task_path, buf, sizeof(buf)) && load_task_parse(buf, &task);
    if (cpu_ok && task_ok) {
        if (have_was) {
            int c = load_cpu_pct(&cpu_was, &cpu), g = load_task_pct(task_was, task, &cpu_was, &cpu);

            if (c >= 0) cpu_pct = c;    /* no tick between the two: the last value stays */
            if (g >= 0) gui_pct = g;
        }
        cpu_was  = cpu;
        task_was = task;
        have_was = true;
    }
    bar_set(BAR_CPU, cpu_pct, &hold_cpu);
    bar_set(BAR_GUI, gui_pct, &hold_gui);
    if (load_read("/proc/meminfo", buf, sizeof(buf)) && load_mem_parse(buf, &mem)) {
        mem_pct = load_mem_pct(&mem);
        bar_set(BAR_MEM, mem_pct, NULL);
        if (mem.swap_total) {
            bar_set(BAR_SWAP, load_swap_pct(&mem), NULL);
        } else {
            cur.px[BAR_SWAP] = -1;
        }
    }
    cur.late = (uint8_t)load_late_level((uint32_t)load_hold_push(&hold_late, busy > INT16_MAX ? INT16_MAX : busy));

    /* In the log only the red dot's: half a second and more without a look at the keys, with the load beside it */
    if (busy >= LOAD_LATE_HIGH_MS) {
        fprintf(stderr, "load: GUI loop late %u ms (processor %d %%, GUI thread %d %% of a core, memory %d %%)\n",
                busy, cpu_pct, gui_pct, mem_pct);
    }

    lv_coord_t w = 2 * PAD + bars_shown() * BAR_W + (bars_shown() - 1) * GAP;

    if (lv_obj_get_width(meter) != w) {
        lv_obj_set_width(meter, w);     /* a swap turned on or off: invalidates both sizes */
    }
    /* Not while nobody sees it: under the screensaver the top layer is hidden, and a dark screen shows nothing. The
     * first second it is seen again draws what changed meanwhile */
    if (backlight_seen() && memcmp(&cur, &drawn, sizeof(cur)) != 0) {
        drawn = cur;
        lv_obj_invalidate(meter);
    }
}

static void start(void) {
    snprintf(task_path, sizeof(task_path), "/proc/self/task/%ld/stat", (long)syscall(SYS_gettid));
    memset(&cur, 0, sizeof(cur));
    cur.px[BAR_SWAP] = -1;
    memset(&drawn, 0xff, sizeof(drawn));    /* nothing drawn yet */
    memset(&hold_cpu, 0, sizeof(hold_cpu));
    memset(&hold_gui, 0, sizeof(hold_gui));
    memset(&hold_late, 0, sizeof(hold_late));
    have_was = false;
    cpu_pct = gui_pct = mem_pct = 0;
    busy_max = 0;

    meter = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(meter);
    lv_obj_clear_flag(meter, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(meter, X, Y);
    lv_obj_set_size(meter, 2 * PAD + (BARS - 1) * BAR_W + (BARS - 2) * GAP, BOX_H);
    lv_obj_add_event_cb(meter, draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    timer = lv_timer_create(sample_cb, PERIOD_MS, NULL);
    sample_cb(timer);   /* the memory at once; the processor from the second read */
}

static void stop(void) {
    lv_timer_del(timer);
    timer = NULL;
    lv_obj_del(meter);
    meter = NULL;
}

static void setting_cb(Subject *subj, void *user_data) {
    bool on = param_i_get(cfg_load_meter) != 0;

    (void)subj;
    (void)user_data;
    if (on && !timer) {
        start();
    } else if (!on && timer) {
        stop();
    }
}

void load_meter_init(void) {
    subject_subscribe_delayed_and_notify((Subject *)cfg_load_meter, setting_cb, NULL);
}
