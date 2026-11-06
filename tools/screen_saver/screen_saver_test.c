/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the screensaver's picture and of what screen_light.c does with the stages. The real
 *  src/screen_saver.c, screen_light.c, screen_idle.c, mono.c and the font, on the PC build's LVGL: an 800 x 480
 *  display flushed into a picture here, as the radio's (a full screen draw buffer, its clear_cb), the GUI one red
 *  object, the time stepped by LVGL's refresh period. Each run in a process of its own (LVGL once a process, the
 *  rain the same from the same ticks).
 *
 *  The rain: drawn the PC build's way (screen_transp 0) and the radios' (1, the GUI over the waterfall plane), green
 *  and 1bit. The radios' picture the PC's, give or take the rounding of the fades, and opaque all over; the picture
 *  made of the frames' areas the whole one drawn again (the head and the tail are all a step changes); 1bit in its
 *  two colours. The radios' path is the one the PC build never runs: there LVGL 8.3's fill_argb lost its place in a
 *  mask wider than the area blended, and every bright glyph came out sheared.
 *
 *  The stages, both ways: LVGL's invalidation once a change (a viewer coming and going while dark left it on), the
 *  GUI drawn under the screensaver as it goes dark (not the rain shown at the next wake), the waterfall and the
 *  spectrum asked to draw when seen again, the top layer hidden under it, the SWR scan (no TX event) and a screen
 *  recording holding it off, TX and the low battery's notice ending it, the knob turn that ends it swallowed for
 *  half a second, a hand mic key not. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "lvgl/lvgl.h"
#include "mono.h"
#include "params/params.h"
#include "radio.h"
#include "screen_idle.h"
#include "screen_light.h"
#include "theme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define W       800
#define H       480
#define STEP    33          /* ms a loop: LVGL's refresh period */
#define SEC     1000u
#define MIN     (60 * SEC)
#define GUI     0xff0000    /* the GUI under the screensaver: red all over */
#define RAIN    0x00ff40    /* the default theme's saver, saver_head, saver_bg */
#define HEAD    0xc8ffc8
#define INK     0x000000    /* 1bit's */
#define PAPER   0xefece2

/* ---- What screen_light.c and screen_saver.c call, stood in for ---- */

params_t      params;
theme_shape_t theme_shape;

static uint64_t      clock_ms = 1000;
static radio_state_t state    = RADIO_RX;
static bool          recording, onebit;
static int           wf_redraws, sp_redraws;

uint64_t get_time() {
    return clock_ms;
}

/* LVGL's clock: the GUI's reads CLOCK_MONOTONIC (lv_conf.h's LV_TICK_CUSTOM, #182), the test's is its own, stepped by
 * run(), so the rain is the same from the same steps. With both defined here the library's lv_hal_tick.o is not linked */
static uint32_t lv_ms;

uint32_t lv_tick_get(void) {
    return lv_ms;
}

uint32_t lv_tick_elaps(uint32_t prev_tick) {
    return lv_ms - prev_tick;   /* across the wrap too, as LVGL's */
}

radio_state_t radio_get_state() {
    return state;
}

void radio_backlight_power(bool on) {
    (void)on;
}

void params_lock() {}

void params_unlock(bool *dirty) {
    (void)dirty;
}

bool screen_rec_is_on() {
    return recording;
}

void waterfall_redraw() {
    wf_redraws++;
}

void spectrum_redraw() {
    sp_redraws++;
}

/* The default theme's screensaver, 1bit's paper on the ink; the rest of the GUI red */
uint32_t theme_hex(theme_color_t c) {
    switch (c) {
        case TC_saver_bg:
            return onebit ? INK : 0x000000;
        case TC_saver:
            return onebit ? PAPER : RAIN;
        case TC_saver_head:
            return onebit ? PAPER : HEAD;
        default:
            return GUI;
    }
}

/* ---- The results, shared with the runs ---- */

typedef struct {
    int      cases, good;
    uint32_t pic[4][W * H];     /* the rain: PC green, radio green, PC 1bit, radio 1bit */
    uint32_t whole[4][W * H];   /* the same drawn again whole */
} shared_t;

static shared_t *out;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    out->cases++;
    out->good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ---- The display, as gui_main.c's ---- */

static uint32_t      panel[W * H];  /* what was flushed: the overlay's picture, alpha and all */
static lv_disp_drv_t drv;

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    for (int y = a->y1; y <= a->y2; y++) {
        for (int x = a->x1; x <= a->x2; x++) {
            panel[y * W + x] = (px++)->full;
        }
    }
    lv_disp_flush_ready(d);
}

/* gui_main.c's: only the area drawn cleared, packed */
static void clear_cb(lv_disp_drv_t *d, uint8_t *buf, uint32_t size) {
    (void)size;
    memset(buf, 0, lv_area_get_size(d->draw_ctx->buf_area) * sizeof(lv_color_t));
}

static void display(bool transp, bool mono) {
    static lv_disp_draw_buf_t draw_buf;
    lv_color_t               *px = malloc(W * H * sizeof(lv_color_t));
    lv_disp_t                *disp;

    onebit           = mono;
    theme_shape.ink  = INK;
    theme_shape.paper = PAPER;
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, px, NULL, W * H);
    lv_disp_drv_init(&drv);
    drv.hor_res       = W;
    drv.ver_res       = H;
    drv.flush_cb      = flush;
    drv.draw_buf      = &draw_buf;
    drv.draw_ctx_init = mono_draw_ctx_init;
    drv.screen_transp = transp;
    drv.clear_cb      = transp ? clear_cb : NULL;
    lv_disp_drv_register(&drv);
    disp = lv_disp_get_default();
    lv_disp_set_bg_color(disp, lv_color_black());
    lv_disp_set_bg_opa(disp, LV_OPA_COVER);
    mono_set(mono);

    lv_obj_t *gui = lv_obj_create(lv_scr_act());

    lv_obj_remove_style_all(gui);
    lv_obj_set_size(gui, W, H);
    lv_obj_set_style_bg_color(gui, lv_color_hex(GUI), 0);
    lv_obj_set_style_bg_opa(gui, LV_OPA_COVER, 0);
    lv_label_set_text(lv_label_create(lv_layer_top()), "");     /* a message on the top layer, of no ink */
}

static void settings(uint8_t saver, uint16_t saver_min, uint16_t off_min) {
    params.brightness_normal  = 5;
    params.brightness_idle    = 4;
    params.brightness_timeout = 30;
    params.brightness_buttons = BUTTONS_TEMPORARILY;
    params.screen_saver.x     = saver;
    params.screen_saver_min.x = saver_min;
    params.screen_off_min     = off_min;
    params.screen_off_always  = false;
}

/* The GUI loop for ms: true when the screensaver showed meanwhile */
static bool run(uint32_t ms) {
    bool saver = false;

    for (uint32_t t = 0; t < ms; t += STEP) {
        lv_ms    += STEP;
        clock_ms += STEP;
        lv_timer_handler();
        saver = saver || backlight_saver_on();
    }
    return saver;
}

static int32_t inv_count(void) {
    return lv_disp_get_default()->inv_en_cnt;
}

/* Every pixel's colour (alpha left out) the GUI's */
static bool all_gui(void) {
    for (int i = 0; i < W * H; i++) {
        if ((panel[i] & 0xffffff) != GUI) {
            return false;
        }
    }
    return true;
}

/* ---- The rain, drawn one way: 20 s of it after its minute ---- */

static void rain(int k, bool transp, bool mono) {
    display(transp, mono);
    settings(SCREEN_SAVER_MATRIX, 1, 0);
    backlight_init();
    run(MIN + 20 * SEC);
    memcpy(out->pic[k], panel, sizeof(panel));
    lv_disp_enable_invalidation(NULL, true);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    memcpy(out->whole[k], panel, sizeof(panel));
}

static int differing(const uint32_t *a, const uint32_t *b, int tolerance) {
    int n = 0;

    for (int i = 0; i < W * H; i++) {
        for (int s = 0; s < 24; s += 8) {
            if (abs((int)(a[i] >> s & 0xff) - (int)(b[i] >> s & 0xff)) > tolerance) {
                n++;
                break;
            }
        }
    }
    return n;
}

static void rain_checks(void) {
    static const char *name[] = {"PC, green", "radio, green", "PC, 1bit", "radio, 1bit"};
    int                lit = 0, opaque = 0, two = 0, two_radio = 0;

    for (int i = 0; i < W * H; i++) {
        lit += (out->pic[0][i] & 0xffffff) != 0;
        opaque += (out->pic[1][i] >> 24) == 0xff && (out->pic[3][i] >> 24) == 0xff;
        two += (out->pic[2][i] & 0xffffff) == INK || (out->pic[2][i] & 0xffffff) == PAPER;
        two_radio += (out->pic[3][i] & 0xffffff) == INK || (out->pic[3][i] & 0xffffff) == PAPER;
    }
    printf("The rain\n");
    check(lit > 10000, "PC, green: rain on the ground (%d pixels lit)", lit);
    for (int k = 0; k < 4; k++) {
        int n = differing(out->pic[k], out->whole[k], 0);

        check(n == 0, "%s: the frames' areas make the whole picture (%d pixels differ)", name[k], n);
    }
    for (int k = 1; k < 4; k += 2) {
        int n = differing(out->pic[k - 1], out->pic[k], 2);

        check(n == 0, "%s: the PC's picture, the fades' rounding aside (%d pixels differ by more)", name[k], n);
    }
    check(opaque == W * H, "radio: opaque all over, the waterfall plane hidden (%d of %d)", opaque, W * H);
    check(two == W * H && two_radio == W * H, "1bit: ink and paper only (PC %d, radio %d of %d)", two, two_radio,
          W * H);
}

/* ---- The stages, one way ---- */

static void stages(bool transp) {
    int wf, sp;

    printf("The stages, %s\n", transp ? "the radios' screen" : "the PC's");
    display(transp, false);
    settings(SCREEN_SAVER_MATRIX, 5, 30);
    backlight_init();
    run(SEC);
    check(lv_disp_is_invalidation_enabled(NULL) && inv_count() == 1, "lit: the GUI drawn (count %d)", inv_count());
    for (int i = 0; i < 3; i++) {
        backlight_viewers(true);
        backlight_viewers(false);
    }
    check(inv_count() == 1, "lit, a viewer come and gone three times: the count as it was (%d)", inv_count());

    wf = wf_redraws;
    check(!run(5 * MIN - 2 * SEC) && run(2 * SEC), "the screensaver at 5 minutes");
    run(24 * MIN);
    check(!lv_disp_is_invalidation_enabled(NULL), "under it: the GUI not drawn (count %d)", inv_count());
    check(lv_obj_has_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN), "under it: the top layer hidden");
    check(wf_redraws == wf, "under it: the waterfall not asked to draw");

    /* Then dark at 30: the GUI drawn as it goes, the next wake shows it at once */
    check(!backlight_is_dark() && !all_gui(), "29 minutes: the screensaver still");
    run(MIN + 2 * SEC);
    check(backlight_is_dark(), "dark at 30 minutes");
    check(all_gui(), "dark after the screensaver: the GUI in the overlay's buffer, not the rain");
    check(!lv_obj_has_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN), "dark: the top layer back");
    check(!lv_disp_is_invalidation_enabled(NULL) && inv_count() == 0, "dark: nothing drawn (count %d)", inv_count());
    for (int i = 0; i < 3; i++) {
        backlight_viewers(true);
        backlight_viewers(false);
    }
    check(!lv_disp_is_invalidation_enabled(NULL) && inv_count() == 0,
          "dark, a viewer come and gone three times: nothing drawn (count %d)", inv_count());

    wf = wf_redraws, sp = sp_redraws;
    check(backlight_tick(), "a key on the dark screen: for waking");
    check(lv_disp_is_invalidation_enabled(NULL) && inv_count() == 1, "woken: the GUI drawn (count %d)", inv_count());
    check(wf_redraws == wf + 1 && sp_redraws == sp + 1, "woken: the waterfall and the spectrum asked to draw now");
    run(SEC);
    check(wf_redraws == wf + 1, "lit on: not asked again");

    params.screen_off_min = 0;
    backlight_settings_changed();

    /* The SWR scan sends no TX event: the radio's state holds the screensaver off */
    state = RADIO_SWRSCAN;
    check(!run(10 * MIN), "an SWR scan for 10 minutes: no screensaver");
    state = RADIO_RX;
    check(!run(5 * MIN - 2 * SEC) && run(4 * SEC), "the scan stopped: the screensaver 5 minutes on");
    state = RADIO_SWRSCAN;
    run(1100);
    check(!backlight_saver_on(), "an SWR scan under the screensaver: gone within a second");
    state = RADIO_RX;

    /* TX's event ends it at once */
    run(6 * MIN);
    wf = wf_redraws;
    backlight_tx(true);
    check(!backlight_saver_on() && lv_disp_is_invalidation_enabled(NULL), "TX on: the screensaver gone at once");
    check(wf_redraws == wf + 1, "TX on: the waterfall asked to draw now");
    run(SEC);
    backlight_tx(false);

    /* A recording: the screensaver waits, and the GUI is drawn for it while dark */
    recording = true;
    check(!run(10 * MIN), "recording for 10 minutes: no screensaver");
    backlight_switch();
    check(backlight_is_dark() && backlight_seen(), "POWER while recording: dark, the pictures drawn for the file");
    check(lv_disp_is_invalidation_enabled(NULL) && inv_count() == 1, "dark and recording: the GUI drawn (count %d)",
          inv_count());
    recording = false;
    run(1100);
    check(!backlight_seen() && inv_count() == 0, "the recording stopped: dark, nothing drawn (count %d)", inv_count());
    backlight_switch();
    check(!backlight_is_dark() && inv_count() == 1, "POWER again: lit (count %d)", inv_count());
    recording = true;
    run(6 * MIN);
    run(1100);
    check(!backlight_saver_on(), "a recording started: no screensaver");
    recording = false;

    /* The knob turn that ends it: its detents for half a second; a hand mic key only itself */
    run(6 * MIN);
    check(backlight_knob_tick(), "a knob on the screensaver: for waking");
    clock_ms += 300;
    check(backlight_knob_tick(), "the same turn 0.3 s on: swallowed still");
    clock_ms += 300;
    check(!backlight_knob_tick(), "0.6 s on: the knob turns");
    run(6 * MIN);
    check(backlight_saver_tick(), "a hand mic key on the screensaver: for waking");
    check(!backlight_knob_tick() && !backlight_saver_tick(), "then a knob or a key: for the radio");

    /* The low battery's warning: the screensaver goes, a dark screen stays dark */
    run(6 * MIN);
    backlight_notice();
    check(!backlight_saver_on() && !lv_obj_has_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN),
          "a notice on the screensaver: gone, the message seen");
    check(!run(5 * MIN - 2 * SEC), "a notice: the screensaver again only after its minutes");
    backlight_switch();
    backlight_notice();
    check(backlight_is_dark(), "a notice on a dark screen: dark");
}

/* Each in a process of its own */
static void in_child(void (*fn)(int, bool, bool), int k, bool transp, bool mono) {
    fflush(stdout);

    pid_t pid = fork();

    if (pid == 0) {
        fn(k, transp, mono);
        fflush(stdout);
        _exit(0);
    }
    int st = 0;

    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        check(false, "a run ended with status %d", st);
    }
}

static void stages_run(int k, bool transp, bool mono) {
    (void)k, (void)mono;
    stages(transp);
}

int main(void) {
    out = mmap(NULL, sizeof(*out), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (out == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    in_child(rain, 0, false, false);
    in_child(rain, 1, true, false);
    in_child(rain, 2, false, true);
    in_child(rain, 3, true, true);
    rain_checks();
    in_child(stages_run, 0, false, false);
    in_child(stages_run, 0, true, false);

    printf("%d of %d cases ok\n", out->good, out->cases);
    return out->good == out->cases ? 0 : 1;
}
