/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Performance overlay (perf_hud.h)
 */
#include "perf_hud.h"
#include "board.h"
#include "screen_light.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool            on;
static lv_obj_t       *label;
static void          (*prev_monitor)(lv_disp_drv_t *, uint32_t, uint32_t);

static uint32_t        frames, draw_ms_sum, draw_ms_max, loop_ms_max;
static uint32_t        knob_n, knob_ms_sum, knob_ms_max;
static _Atomic uint64_t knob_first;     /* oldest detent not yet drawn, 0 = none */
static uint64_t        knob_used;       /* handed to the GUI, waiting for its frame */
static uint32_t        log_count;

static uint64_t now_us(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000u;
}

bool perf_hud_on(void) {
    return on;
}

void perf_loop_time(uint32_t ms) {
    if (on && ms > loop_ms_max) loop_ms_max = ms;
}

void perf_knob_seen(uint64_t t_us) {
    uint64_t zero = 0;

    if (on) atomic_compare_exchange_strong(&knob_first, &zero, t_us);
}

void perf_knob_used(void) {
    if (on && !knob_used) knob_used = atomic_exchange(&knob_first, 0);
}

void perf_knob_drop(void) {
    if (on) atomic_store(&knob_first, 0);
}

/* LVGL calls it after each refresh: time spent drawing and pixels drawn */
static void monitor_cb(lv_disp_drv_t *drv, uint32_t time, uint32_t px) {
    frames++;
    draw_ms_sum += time;
    if (time > draw_ms_max) draw_ms_max = time;
    if (knob_used) {
        uint32_t ms = (uint32_t)((now_us() - knob_used) / 1000);

        knob_n++;
        knob_ms_sum += ms;
        if (ms > knob_ms_max) knob_ms_max = ms;
        knob_used = 0;
    }
    if (prev_monitor) prev_monitor(drv, time, px);
}

static void show_cb(lv_timer_t *t) {
    static uint64_t last;
    uint64_t        now = now_us();
    float           s = last ? (now - last) / 1e6f : 1.0f;
    char            text[160];

    last = now;
    snprintf(text, sizeof(text), "%.0f fps  draw %.1f/%u ms  loop %u ms  knob %s",
             frames / s, frames ? (float)draw_ms_sum / frames : 0.0f, draw_ms_max, loop_ms_max, "");
    if (knob_n) {
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "%u/%u ms", knob_ms_sum / knob_n, knob_ms_max);
    } else {
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "-");
    }
    if (backlight_seen()) {     /* the label redrawn only where someone sees it; the log goes on */
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(label, text);
        lv_obj_move_foreground(label);
    } else {                    /* not left over the screensaver: its rain draws over where it was */
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    if (++log_count % 10 == 0) fprintf(stderr, "perf: %s\n", text);
    frames = draw_ms_sum = draw_ms_max = loop_ms_max = 0;
    knob_n = knob_ms_sum = knob_ms_max = 0;
}

void perf_hud_init(lv_disp_drv_t *drv) {
    const char *env = getenv("X6100_PERF");

    on = (env && *env == '1') || access(BOARD_DATA "/perf", F_OK) == 0;
    if (!on) return;
    prev_monitor = drv->monitor_cb;
    drv->monitor_cb = monitor_cb;
    lv_disp_drv_update(lv_disp_get_default(), drv);

    label = lv_label_create(lv_layer_sys());
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x7dff9a), 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_pad_hor(label, 4, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_RIGHT, 0, -62);
    lv_label_set_text(label, "perf");
    lv_timer_create(show_cb, 1000, NULL);
    fprintf(stderr, "perf: overlay on\n");
}
