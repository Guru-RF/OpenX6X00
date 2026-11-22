/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the remote screen's picture with the DRM planes (lv_drivers/display/drm.c): the GUI's
 *  frame completed with the waterfall plane's picture under its see-through parts.
 *
 *  The remote page's frames were completed on the GUI thread, at every frame: for each of the 800 x 480 pixels the
 *  plane's pixel was fetched (along a landscape row they are a panel row, 1920 bytes, apart) and blended. Now the
 *  GUI thread copies the plane's picture as it is (drm_under_copy()) and the encoder thread completes the frame
 *  from that copy (drm_compose_landscape_from()), reading the plane's picture only where the GUI is see-through.
 *
 *  The real drm.c, compiled for the PC with the planes' code in (USE_DRM=1; nothing of DRM is opened). Checked,
 *  against the loop as it was (the plane's pixels of a row gathered, then drm_blend_under() over the whole row), on
 *  GUI frames of opaque, see-through and partly see-through pixels over a plane of random pixels:
 *  - a frame like the main screen (the waterfall and spectrum see-through, the rest opaque, soft edges between)
 *  - a frame of random alpha at every pixel
 *  - a wholly opaque frame (a window over everything) and a wholly see-through one
 *  the same picture pixel for pixel; and the time of both on this PC for the main screen's frame.
 *  See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "lv_drivers/display/drm.h"
#include "board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LAND_W  BOARD_DISP_W
#define LAND_H  BOARD_DISP_H
#define PANEL_W LAND_H
#define PANEL_H LAND_W

uint32_t lv_tick_mono_now;      /* LVGL's clock (lv_tick_mono.h) */

static uint32_t under[PANEL_W * PANEL_H], gui[LAND_W * LAND_H], was[LAND_W * LAND_H], now[LAND_W * LAND_H];
static int      cases, good;

static uint32_t rnd_s = 2463534242u;

static uint32_t rnd(void) {
    rnd_s ^= rnd_s << 13;
    rnd_s ^= rnd_s >> 17;
    rnd_s ^= rnd_s << 5;
    return rnd_s;
}

/* As drm_compose_landscape() was: the plane's pixels of a landscape row gathered, the GUI's row blended over them */
static void compose_was(uint32_t *px) {
    for (int32_t y = 0; y < LAND_H; y++) {
        uint32_t *row = px + y * LAND_W;
        uint32_t  u[LAND_W];

        for (int32_t x = 0; x < LAND_W; x++) {
            u[x] = under[(LAND_W - 1 - x) * PANEL_W + y];
        }
        drm_blend_under(row, row, u, LAND_W);
    }
}

static double ms_of(void (*fn)(void)) {
    double best = 1e9;

    for (int i = 0; i < 40; i++) {
        struct timespec a, b;

        memcpy(now, gui, sizeof(gui));
        clock_gettime(CLOCK_MONOTONIC, &a);
        fn();
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;

        if (ms < best) best = ms;
    }
    return best;
}

static void run_was(void) { compose_was(now); }
static void run_now(void) { drm_compose_landscape_from(now, under); }

static void same(const char *what) {
    int32_t wrong = 0, first = -1, through = 0;

    memcpy(was, gui, sizeof(gui));
    memcpy(now, gui, sizeof(gui));
    compose_was(was);
    drm_compose_landscape_from(now, under);
    for (int32_t i = 0; i < LAND_W * LAND_H; i++) {
        if (was[i] != now[i] && wrong++ == 0) first = i;
        through += (gui[i] >> 24) != 255;
    }
    cases++;
    good += wrong == 0;
    printf("%s %s: the same picture (%d of %d pixels see-through or partly)\n", wrong ? "  BAD" : "  ok ", what, through,
           LAND_W * LAND_H);
    if (wrong) {
        printf("        %d pixels differ, the first at x %d, y %d: 0x%08x, was 0x%08x\n", wrong, first % LAND_W,
               first / LAND_W, now[first], was[first]);
    }
}

int main(void) {
    for (int32_t i = 0; i < PANEL_W * PANEL_H; i++) under[i] = rnd();   /* the X byte too: it must not show */

    /* The main screen: the top bars opaque, the spectrum and the waterfall see-through, the buttons opaque over the
     * waterfall's bottom, a line and a label over the waterfall, soft edges of partly see-through pixels */
    for (int32_t y = 0; y < LAND_H; y++) {
        for (int32_t x = 0; x < LAND_W; x++) {
            uint32_t c = rnd() & 0xffffff, a = 255;

            if (y >= 100 && y < 420) a = 0;
            if (y >= 98 && y < 100) a = 128;
            if (y >= 100 && y < 420 && x == LAND_W / 2) a = 255;                 /* the VFO line */
            if (y >= 200 && y < 420 && x > 380 && x < 440) a = 90;               /* the filter shade */
            if (y >= 120 && y < 140 && x > 20 && x < 200) a = (x * 7 + y) & 255; /* a label's antialiased text */
            gui[y * LAND_W + x] = a << 24 | c;
        }
    }
    same("the main screen");

    double t_was = ms_of(run_was), t_now = ms_of(run_now);
    double t_copy;
    {
        static uint32_t copy[PANEL_W * PANEL_H];
        struct timespec a, b;

        t_copy = 1e9;
        for (int i = 0; i < 40; i++) {
            clock_gettime(CLOCK_MONOTONIC, &a);
            memcpy(copy, under, sizeof(under));
            clock_gettime(CLOCK_MONOTONIC, &b);
            double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;

            if (ms < t_copy) t_copy = ms;
            if (copy[i] != under[i]) return 2;
        }
    }

    for (int32_t i = 0; i < LAND_W * LAND_H; i++) gui[i] = rnd();
    same("random alpha everywhere");
    for (int32_t i = 0; i < LAND_W * LAND_H; i++) gui[i] = rnd() | 0xff000000;
    same("all opaque");
    for (int32_t i = 0; i < LAND_W * LAND_H; i++) gui[i] = rnd() & 0xffffff;
    same("all see-through");

    printf("time  the main screen's frame on this PC: on the GUI thread %.2f ms before, %.2f ms now (the plane's picture\n"
           "      copied); on the encoder thread %.2f ms\n", t_was, t_copy, t_now);
    printf("%d of %d cases ok\n", good, cases);
    return good == cases ? 0 : 1;
}
