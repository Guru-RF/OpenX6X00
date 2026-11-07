/* 3D waterfall: the landscape picture drawn as columns and turned over (d3_render) vs drawn in rows (before),
 * pixel for pixel. The code under test is extracted from src/waterfall_view.c by d3_colmajor_test.sh (from
 * "#define D3_STEP" to the labels); the DSP rows, the settings and the theme around it are stubs here.
 *
 *   d3_colmajor_test OUT   renders the scenarios and writes a checksum line per picture to OUT
 * The script builds it from the old code (-DOLD, which draws a landscape picture in rows: git show of the
 * commit before) and from the new, and compares the two OUT files; both also dump a bench time. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "board.h"                 /* the panel of the board built for (BOARD_DEFS in the script) */
#define WIDTH       BOARD_DISP_W
#define MAX_HEIGHT  BOARD_DISP_H
#define ROW_BINS    2048
#define DATA_AT     512
#define WATERFALL_NFFT 1024
#define LV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define LV_MAX(a, b) ((a) > (b) ? (a) : (b))
#define LV_CLAMP(lo, v, hi) LV_MAX(lo, LV_MIN(v, hi))
#define LV_ABS(a) ((a) > 0 ? (a) : -(a))
typedef int16_t lv_coord_t;
typedef struct { int dummy; } lv_obj_t;
typedef struct { int dummy; } lv_timer_t;
typedef struct {
    uint8_t  values[ROW_BINS];
    uint32_t center_freq, width;
    uint8_t  floor, seq;
    uint32_t num, ms;
    bool     kept;
} wf_row_t;

static lv_coord_t height;
static wf_row_t  *rows;
static uint16_t   newest;
static int32_t    wf_center_freq = 7074000;
static lv_obj_t  *obj;
static bool       theme_amber;
static struct { bool mono; uint32_t ink, paper; } theme_shape;
static uint32_t   wf_palette[256];
static int32_t    buttons_h = 40;
static bool       buttons_hidden(void) { return false; }
static uint32_t   fake_ms;
static uint32_t   get_time(void) { return fake_ms; }
static uint32_t   lv_tick_get(void) { return fake_ms; }
static wf_row_t  *row_from(uint16_t top, int32_t age) { int32_t ring = height + 1; return &rows[(top - age + ring) % ring]; }
static uint32_t   shown_span(void) { return 100000; }
static void       waterfall_get_levels(bool a, float *min, float *max) { (void) a; *min = 20; *max = 200; }

enum { cfg_wf_3d_tilt, cfg_wf_3d_floor, cfg_wf_3d_depth, cfg_wf_3d_peak, cfg_wf_3d_colors, cfg_wf_3d_range, cfg_wf_3d_glide, cfg_wf_3d, N_CFG };
static int32_t cfg[N_CFG];
static int32_t param_i_get(int k) { return cfg[k]; }
typedef struct { uint32_t full; } lv_color_t;
static uint32_t panel_hex = 0x101820;
#define TC(role) ((lv_color_t){panel_hex})
static void theme_ramp_palette(const uint32_t *stops, int n, int steps, uint32_t *out) {
    (void) steps;
    for (int i = 0; i < 256; i++) {
        float f = i / 255.0f * (n - 1); int k = (int) f; if (k >= n - 1) k = n - 2; float t = f - k;
        uint32_t a = stops[k], b = stops[k + 1], c = 0;
        for (int s = 0; s <= 16; s += 8) c |= (uint32_t)(((a >> s & 255) * (1 - t) + (b >> s & 255) * t)) << s;
        out[i] = c | 0xff000000u;
    }
}
/* the DSP rows' pixels: the row's values spread over the span (a stand-in for row_pixels()) */
static void row_pixels(const wf_row_t *row, uint32_t center, uint32_t span, size_t i0, size_t i1, uint8_t *out) {
    (void) center; (void) span;
    for (size_t i = i0; i < i1; i++) out[i - i0] = row->values[DATA_AT + (i * WATERFALL_NFFT / WIDTH) % WATERFALL_NFFT];
}

#include "d3_extract.inc"

static uint64_t fnv(const void *p, size_t n) {
    const uint8_t *b = p; uint64_t h = 1469598103934665603ull;
    while (n--) h = (h ^ *b++) * 1099511628211ull;
    return h;
}
static uint32_t rnd_s = 12345;
static uint32_t rnd(void) { rnd_s = rnd_s * 1664525u + 1013904223u; return rnd_s >> 8; }

static void make_rows(int h, int density) {
    for (int i = 0; i <= h; i++) {
        wf_row_t *r = &rows[i];
        r->width = 100000; r->center_freq = 7074000; r->floor = 40 + rnd() % 20; r->num = i; r->ms = i * 1000;
        for (int k = 0; k < ROW_BINS; k++) {
            int v = 20 + rnd() % 40;
            if (rnd() % 1000 < (unsigned) density) v = 60 + rnd() % 190;
            if ((k % 211) < 5) v = 200 + (i * 7) % 50;     /* carriers that persist across rows */
            r->values[k] = LV_MIN(v, 255);
        }
    }
    newest = h;
}

static int dump_all;
int main(int argc, char **argv) {
    FILE *out = fopen(argc > 1 ? argv[1] : "/dev/null", "w");
    int bench = argc > 2;
    rows = calloc(MAX_HEIGHT + 1, sizeof(*rows));
    for (int i = 0; i < 256; i++) wf_palette[i] = 0xff000000u | (i * 0x010203u);
    int hs[] = {MAX_HEIGHT * 62 / 100, MAX_HEIGHT / 2, MAX_HEIGHT - 1, MAX_HEIGHT / 2 + 1, MAX_HEIGHT / 5};
    int pics = 0;
    for (int hi = 0; hi < 5; hi++)
    for (int mono = 0; mono < 2; mono++)
    for (int colors = 0; colors < 3; colors++) {
        height = hs[hi];
        theme_shape.mono = mono; theme_shape.ink = 0xe0e0e0; theme_shape.paper = 0x101010;
        cfg[cfg_wf_3d_tilt] = (hi + colors) % 8; cfg[cfg_wf_3d_floor] = colors * 3; cfg[cfg_wf_3d_depth] = 8 + hi * 8;
        cfg[cfg_wf_3d_peak] = mono ? 1 : colors & 1; cfg[cfg_wf_3d_colors] = colors; cfg[cfg_wf_3d_range] = 40 + hi * 10;
        make_rows(height, 4 + hi * 6);
        static uint32_t dst_a[WIDTH * MAX_HEIGHT], dst_b[WIDTH * MAX_HEIGHT];
        int32_t ref_x[3] = {WIDTH / 10, WIDTH / 2, WIDTH * 88 / 100};
        d3_vanish = hi == 2 ? WIDTH * 3 / 10 : WIDTH / 2;
        /* a sequence: full, same scene again (dirty-column restore), new rows, moved reference lines, a changed
         * target buffer (2D drew in it) */
        for (int step = 0; step < 8; step++) {
            uint32_t *dst = (step & 1) ? dst_b : dst_a;
            if (step == 3) { for (int i = 0; i < 5; i++) { newest = (newest + 1) % (height + 1); rows[newest].num = rows[(newest + height) % (height + 1)].num + 1; for (int k = 0; k < ROW_BINS; k++) rows[newest].values[k] = 20 + rnd() % 60 + ((k % 97) < 3 ? 150 : 0); } }
            if (step == 5) ref_x[1] = WIDTH * 4 / 10;
            if (step == 6) d3_drawn_buf = NULL;
            fake_ms += 33;
            memset(dst, 0x5a, sizeof(uint32_t) * WIDTH * height);   /* what was in the buffer before is not kept */
            if (step == 7) memset(dst, 0, 4);
            d3_render(dst, false, ref_x, 3);
            fprintf(out, "h%d mono%d col%d step%d %016llx\n", height, mono, colors, step,
                    (unsigned long long) fnv(dst, sizeof(uint32_t) * WIDTH * height));
            pics++;
        }
        /* portrait target (a portrait build): the same picture, written to a portrait buffer */
        static uint32_t pb[WIDTH * MAX_HEIGHT];
        d3_render(pb, true, ref_x, 3);
        fprintf(out, "h%d mono%d col%d portrait %016llx\n", height, mono, colors, (unsigned long long) fnv(pb, sizeof(uint32_t) * WIDTH * height));
    }
    if (bench) {
        height = MAX_HEIGHT * 62 / 100; theme_shape.mono = 0; cfg[cfg_wf_3d_tilt] = 3; cfg[cfg_wf_3d_depth] = 120; cfg[cfg_wf_3d_range] = 60;
        cfg[cfg_wf_3d_colors] = 0; cfg[cfg_wf_3d_peak] = 1;
        make_rows(height, 20);
        static uint32_t dst[WIDTH * MAX_HEIGHT]; int32_t ref_x[3] = {WIDTH / 10, WIDTH / 2, WIDTH * 88 / 100};
        double best = 1e9, all[30];
        for (int i = 0; i < 30; i++) {
            struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
            fake_ms += 33; d3_render(dst, false, ref_x, 3);
            clock_gettime(CLOCK_MONOTONIC, &b);
            all[i] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6; if (all[i] < best) best = all[i];
        }
        for (int i = 0; i < 30; i++) for (int j = i + 1; j < 30; j++) if (all[j] < all[i]) { double t = all[i]; all[i] = all[j]; all[j] = t; }
        printf("d3_render landscape %dx%d: min %.2f ms  median %.2f ms\n", WIDTH, height, all[0], all[15]);
    }
    fclose(out);
    fprintf(stderr, "%d landscape pictures\n", pics);
    return 0;
}
