/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Panel stripes fix (Settings > Display > BACKLIGHT, #239)
 *
 *  Some newer X6100s show vertical coloured stripes in the darkest shades, where a colour is 8 to 31 out of 255
 *  (tcon_gamma.h says why the gamma table helps). With the setting on, the GUI fills the display engine's gamma table
 *  with table A and turns it on: at the start, after the first frame's mode set (drm.c), when the panel is lit again
 *  (screen_light.c), and whenever a look every few seconds finds it turned off by something else. Off, the table is
 *  turned off (at the start too, should a GUI before this one have left it on). The engine's registers are reached
 *  through /dev/mem, one page mapped once, and only where the device tree has that TCON there: both radios have
 *  that processor. The colours themselves are never touched, nor the framebuffer: no cost per frame.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "panel_stripes.h"

#include "tcon_gamma.h"
#include "cfg/cfg_api.h"
#include "lvgl/lvgl.h"
#include "lv_drivers/display/drm.h"     /* USE_DRM, from lv_drv_conf.h on the radios */

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define WATCH_MS    3000    /* the table looked at while the fix is on: one register read */
#define DT_COMPAT   "allwinner,sun8i-a33-tcon"     /* the device tree's name for TCON0, X6100 and X6200 alike */
/* The host test (tools/panel_stripes) gives files of its own */
#ifndef DT_TCON
#define DT_TCON     "/proc/device-tree/soc/lcd-controller@1c0c000/compatible"
#endif
#ifndef DEV_MEM
#define DEV_MEM     "/dev/mem"
#endif

static volatile uint32_t *tcon;         /* TCON0's page; NULL: not mapped */
static bool               looked;       /* the device tree and /dev/mem tried, once */
static int                said = -1;    /* the last tcon_gamma_t the log was given; none: off from the start */

/* TCON0's page, mapped at the first call: only where the device tree says that TCON is there */
static volatile uint32_t *tcon_page(void) {
#ifdef HOST_BUILD
    return NULL;            /* a PC: no such engine */
#else
    char  compat[64] = "";
    FILE *f;
    int   fd;
    void *p;

    if (looked) {
        return tcon;
    }
    looked = true;
    if ((f = fopen(DT_TCON, "r"))) {
        size_t n = fread(compat, 1, sizeof(compat) - 1, f);

        compat[n] = '\0';   /* the first of its names, the one that counts */
        fclose(f);
    }
    if (strcmp(compat, DT_COMPAT) != 0) {
        LV_LOG_USER("panel: no TCON0 in the device tree, the stripes fix does nothing here");
        return NULL;
    }
    fd = open(DEV_MEM, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        LV_LOG_USER("panel: /dev/mem not opened, the stripes fix does nothing");
        return NULL;
    }
    p = mmap(NULL, TCON0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, TCON0_BASE);
    close(fd);
    if (p == MAP_FAILED) {
        LV_LOG_USER("panel: the TCON not mapped, the stripes fix does nothing");
        return NULL;
    }
    tcon = p;
    return tcon;
#endif
}

/* The setting on the engine now. Off at the start reads the global control only, and writes it only when a GUI
 * before this one left the table on */
static void apply(void) {
    bool               on = param_i_get(cfg_panel_stripes);
    volatile uint32_t *t;
    tcon_gamma_t       r;

    t = tcon_page();
    if (!t) {
        return;
    }
    r = tcon_gamma_set(t, on);
    if ((int)r != said && (on || said >= 0)) {     /* a line when it changes; none while off from the start */
        said = r;
        LV_LOG_USER("panel: stripes fix %s", r == TCON_GAMMA_ON ? "on, gamma table A" :
                    r == TCON_GAMMA_OFF ? "off" : "waits: the TCON is off");
    }
}

static void setting_cb(Subject *subj, void *user_data) {
    apply();
}

/* While on: the table back on when something turned it off (a mode set, the engine set up again) */
static void watch_cb(lv_timer_t *timer) {
    if (tcon && param_i_get(cfg_panel_stripes) && tcon_gamma_lost(tcon)) {
        LV_LOG_USER("panel: the gamma table was off, on again");
        apply();
    }
}

void panel_stripes_init(void) {
    apply();
    subject_subscribe((Subject *)cfg_panel_stripes, setting_cb, NULL);
#if USE_DRM
    drm_set_modeset_cb(panel_stripes_again);
#endif
    lv_timer_create(watch_cb, WATCH_MS, NULL);
}

void panel_stripes_again(void) {
    if (param_i_get(cfg_panel_stripes)) {
        apply();
    }
}
