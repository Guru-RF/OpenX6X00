/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the Panel stripes fix as the GUI runs it (src/panel_stripes.c, #239): the device tree and
 *  /dev/mem are files of the test (test.d/compatible, and test.d/mem, as large as the address of the TCON's page and
 *  sparse), so the page the module maps is the test's to look at. In order: off from the start leaves the TCON as it
 *  is and says nothing; a table a GUI before this one left on is turned off at the start; the setting turned on fills
 *  table A and turns it on, said in the log; the first frame's mode set, a wake and the watch put it back after
 *  something turned it off; off takes it away; nothing is written while the TCON is off, and the watch waits for it;
 *  with no TCON0 in the device tree nothing is mapped and the log says so. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE
#include "build/src/panel_stripes.h"
#include "build/src/tcon_gamma.h"
#include "cfg/cfg_api.h"
#include "lvgl/lvgl.h"
#include "lv_drivers/display/drm.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failed;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* ---- What panel_stripes.c calls, stood in for ---- */

static int32_t      setting;            /* Panel stripes fix */
static observer_cb  setting_cb;
static void        (*modeset_cb)(void);
static lv_timer_cb_t watch_cb;
static uint32_t      watch_ms;
static char          log_text[4096];

ParamInt *cfg_panel_stripes = (ParamInt *)&setting;

int32_t param_i_get(const ParamInt *p) {
    return *(const int32_t *)p;
}

Observer *subject_subscribe(Subject *subj, observer_cb fn, void *user_data) {
    (void)subj;
    (void)user_data;
    setting_cb = fn;
    return NULL;
}

void drm_set_modeset_cb(void (*cb)(void)) {
    modeset_cb = cb;
}

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data) {
    (void)user_data;
    watch_cb = cb;
    watch_ms = period;
    return NULL;
}

void test_log(const char *fmt, ...) {
    size_t  n = strlen(log_text);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(log_text + n, sizeof(log_text) - n, fmt, ap);
    va_end(ap);
    n = strlen(log_text);
    snprintf(log_text + n, sizeof(log_text) - n, "\n");
}

static bool said(const char *what) {
    return strstr(log_text, what) != NULL;
}

/* ---- The fake device tree and memory ---- */

static volatile uint32_t *page;     /* the TCON's page of test.d/mem, as the module sees it */

static void files(bool tcon) {
    FILE *f;
    int   fd;

    if (system("rm -rf test.d && mkdir -p test.d") != 0) {
        exit(2);
    }
    if (tcon) {
        f = fopen("test.d/compatible", "w");
        fwrite("allwinner,sun8i-a33-tcon", 1, sizeof("allwinner,sun8i-a33-tcon"), f);    /* with its NUL */
        fclose(f);
    }
    fd = open("test.d/mem", O_RDWR | O_CREAT, 0644);
    if (fd < 0 || ftruncate(fd, TCON0_BASE + TCON0_SIZE) != 0) {
        perror("test.d/mem");
        exit(2);
    }
    page = mmap(NULL, TCON0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, TCON0_BASE);
    close(fd);
    if (page == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
}

/* The engine as the kernel leaves it: this global control, the table random */
static void engine(uint32_t gctl) {
    uint32_t x = 0x001D0FA5u;

    for (int i = 0; i < TCON_GAMMA_N; i++) {
        x = x * 1664525u + 1013904223u;
        page[0x100 + i] = x & 0x00FFFFFFu;
    }
    page[0] = gctl;
}

static bool table_a(void) {
    for (int i = 0; i < TCON_GAMMA_N; i++) {
        if (page[0x100 + i] != tcon_gamma_entry(i)) {
            return false;
        }
    }
    return true;
}

static bool table_random(void) {
    int a = 0;

    for (int i = 0; i < TCON_GAMMA_N; i++) {
        a += page[0x100 + i] == tcon_gamma_entry(i);
    }
    return a < 8;
}

static void set(int32_t on) {
    setting = on;
    setting_cb(NULL, NULL);
}

/* No TCON0 in the device tree: nothing mapped, the setting on does nothing (a child: the module looks once) */
static int no_tcon(void) {
    files(false);
    engine(0x80000000u);
    setting = 1;
    panel_stripes_init();
    check(page[0] == 0x80000000u && table_random(), "no TCON0 in the device tree: on does nothing");
    check(said("no TCON0 in the device tree"), "the log says so");
    panel_stripes_again();
    watch_cb(NULL);
    check(page[0] == 0x80000000u && table_random(), "a wake and the watch: nothing either");
    return failed;
}

/* Off at the start, the table on: a GUI before this one left it so (a child: a start of its own) */
static int left_on(void) {
    files(true);
    engine(0xC0000000u);
    page[0x100] = 0x123456;
    setting = 0;
    panel_stripes_init();
    check(page[0] == 0x80000000u && page[0x100] == 0x123456, "off at the start, the table left on: turned off");
    check(log_text[0] == '\0', "nothing in the log");
    return failed;
}

/* A start of its own in a child, as the module looks at the device tree once a run */
static void in_child(int (*fn)(void), const char *what) {
    pid_t child = fork();
    int   st;

    if (child == 0) {
        exit(fn());
    }
    waitpid(child, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, what);
}

int main(void) {
    in_child(no_tcon, "no TCON0 (a start of its own)");
    in_child(left_on, "the table left on (a start of its own)");

    files(true);

    /* Off from the start: the global control read, nothing written, nothing said */
    engine(0x80000000u);
    setting = 0;
    panel_stripes_init();
    check(setting_cb && modeset_cb && watch_cb && watch_ms == 3000, "the setting, the mode set and a 3 s watch followed");
    check(page[0] == 0x80000000u && table_random(), "off from the start: the TCON as it was");
    check(log_text[0] == '\0', "off from the start: nothing in the log");
    modeset_cb();
    panel_stripes_again();
    watch_cb(NULL);
    check(page[0] == 0x80000000u && table_random(), "off: the mode set, a wake and the watch touch nothing");

    /* On */
    set(1);
    check(page[0] == 0xC0000000u && table_a(), "turned on: table A, then the table on");
    check(said("panel: stripes fix on, gamma table A"), "said in the log");

    /* The first frame's mode set, as if it had reset the engine */
    engine(0x80000000u);
    modeset_cb();
    check(page[0] == 0xC0000000u && table_a(), "after the mode set: table A on again");

    /* The panel lit again */
    engine(0x80000000u);
    panel_stripes_again();
    check(page[0] == 0xC0000000u && table_a(), "a wake: table A on again");

    /* The watch: the table on, left alone; turned off by something else, on again and said */
    page[0x100] = 0x123456;     /* a table that is on cannot be read back: not refilled while its bit is there */
    watch_cb(NULL);
    check(page[0x100] == 0x123456, "the watch, the table on: left alone");
    engine(0x80000000u);
    watch_cb(NULL);
    check(page[0] == 0xC0000000u && table_a() && said("the gamma table was off, on again"),
          "the watch, the table off: on again, said");

    /* Off */
    set(0);
    check(page[0] == 0x80000000u && said("panel: stripes fix off"), "turned off: the table off, said");
    engine(0x80000000u);
    panel_stripes_again();
    watch_cb(NULL);
    check(page[0] == 0x80000000u && table_random(), "off: a wake and the watch put nothing on");

    /* The TCON off (the panel off by the kernel): nothing written, the watch waits for it */
    engine(0x00000000u);
    set(1);
    check(page[0] == 0x00000000u && table_random(), "the TCON off: nothing written");
    check(said("waits: the TCON is off"), "said in the log");
    watch_cb(NULL);
    check(page[0] == 0x00000000u, "the watch, the TCON still off: nothing");
    page[0] = 0x80000000u;
    watch_cb(NULL);
    check(page[0] == 0xC0000000u && table_a(), "the TCON on: the watch puts table A on");

    printf("panel_stripes: %d checks, %d failed\n", checks, failed);
    if (failed == 0 && system("rm -rf test.d") != 0) {
        return 2;
    }
    return failed != 0;
}
