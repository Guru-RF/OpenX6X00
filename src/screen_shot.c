/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The screen to /mnt/<date time>.png: the picture is taken in the GUI thread, written by a
 * thread of its own */

#include "screen_shot.h"
#include "lvgl/lvgl.h"
#include "lv_drivers/display/drm.h"
#include "util.h"
#include "msg_box.h"

#include <png.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static lv_img_dsc_t snap;
static uint8_t     *argb;           /* the snapshot, 4 bytes a pixel, B G R A */
static uint8_t     *rows[800];      /* 480 landscape, 800 portrait (walkie) */

static bool write_png(FILE *f, uint16_t w, uint16_t h) {
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info;

    if (!png) {
        LV_LOG_ERROR("Create write struct");
        return false;
    }
    info = png_create_info_struct(png);
    if (!info) {
        LV_LOG_ERROR("Create info struct");
        png_destroy_write_struct(&png, NULL);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {      /* libpng failed while writing */
        LV_LOG_ERROR("Set jump");
        png_destroy_write_struct(&png, NULL);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

    for (uint16_t y = 0; y < h; y++) {
        const uint8_t *px = argb + (size_t)y * w * 4;

        rows[y] = malloc(w * 3);
        for (uint16_t x = 0; x < w; x++, px += 4) {
            rows[y][x * 3]     = px[2];
            rows[y][x * 3 + 1] = px[1];
            rows[y][x * 3 + 2] = px[0];
        }
    }
    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    png_write_end(png, info);
    for (uint16_t y = 0; y < h; y++) {
        free(rows[y]);
    }
    png_destroy_write_struct(&png, NULL);
    return true;
}

static void *screenshot_thread(void *arg) {
    char  when[64], path[80];
    FILE *f;

    get_time_str(when, sizeof(when));
    snprintf(path, sizeof(path), "/mnt/%s.png", when);
    f = fopen(path, "wb");
    if (!f) {
        msg_update_text_fmt("Error write file");
    } else {
        if (write_png(f, snap.header.w, snap.header.h)) {
            msg_update_text_fmt("Saved %s", when);
        }
        fclose(f);
    }
    free(argb);
    return NULL;
}

void screenshot_take() {
    uint32_t  size = lv_snapshot_buf_size_needed(lv_scr_act(), LV_IMG_CF_TRUE_COLOR_ALPHA);
    pthread_t thread;

    argb = malloc(size);
    lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR_ALPHA, &snap, argb, size);
#if USE_DRM
    /* the waterfall is on a plane of its own, under the see-through GUI */
    if (snap.header.w == 800 && snap.header.h == 480) {
        drm_compose_landscape((uint32_t *)argb);
    }
#endif
    pthread_create(&thread, NULL, screenshot_thread, NULL);
    pthread_detach(thread);
}
