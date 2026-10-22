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

/* One screenshot: its own buffers, so a second one taken while the first is still being written
 * shares nothing with it (audit A06) */
typedef struct {
    lv_img_dsc_t snap;
    uint8_t     *argb;              /* the snapshot, 4 bytes a pixel, B G R A */
    uint8_t    **rows;              /* one RGB row a pointer, h of them */
} shot_t;

static bool write_png(FILE *f, shot_t *s) {
    uint16_t    w   = s->snap.header.w, h = s->snap.header.h;
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
        const uint8_t *px  = s->argb + (size_t)y * w * 4;
        uint8_t       *row = s->rows[y];

        for (uint16_t x = 0; x < w; x++, px += 4) {
            row[x * 3]     = px[2];
            row[x * 3 + 1] = px[1];
            row[x * 3 + 2] = px[0];
        }
    }
    png_set_rows(png, info, s->rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    png_write_end(png, info);
    png_destroy_write_struct(&png, NULL);
    return true;
}

static void shot_free(shot_t *s) {
    if (s->rows) {
        for (uint16_t y = 0; y < s->snap.header.h; y++)
            free(s->rows[y]);
        free(s->rows);
    }
    free(s->argb);
    free(s);
}

static void *screenshot_thread(void *arg) {
    shot_t *s = (shot_t *)arg;
    char    when[64], path[80];
    FILE   *f;

    get_time_str(when, sizeof(when));
    snprintf(path, sizeof(path), "/mnt/%s.png", when);
    f = fopen(path, "wb");
    if (!f) {
        msg_update_text_fmt("Error write file");
    } else {
        if (write_png(f, s)) {
            msg_update_text_fmt("Saved %s", when);
        }
        fclose(f);
    }
    shot_free(s);
    return NULL;
}

void screenshot_take() {
    uint32_t  size = lv_snapshot_buf_size_needed(lv_scr_act(), LV_IMG_CF_TRUE_COLOR_ALPHA);
    pthread_t thread;
    shot_t   *s = calloc(1, sizeof(*s));

    if (s)
        s->argb = malloc(size);
    if (!s || !s->argb || lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR_ALPHA, &s->snap, s->argb, size) != LV_RES_OK) {
        LV_LOG_ERROR("Screenshot: no memory");
        if (s)
            shot_free(s);
        return;
    }
    /* the rows for the PNG, taken here so the writer has nothing left to fail on */
    s->rows = calloc(s->snap.header.h, sizeof(uint8_t *));
    for (uint16_t y = 0; s->rows && y < s->snap.header.h; y++) {
        s->rows[y] = malloc((size_t)s->snap.header.w * 3);
        if (!s->rows[y]) {
            LV_LOG_ERROR("Screenshot: no memory");
            shot_free(s);
            return;
        }
    }
    if (!s->rows) {
        shot_free(s);
        return;
    }
#if USE_DRM
    /* the waterfall is on a plane of its own, under the see-through GUI */
    if (s->snap.header.w == 800 && s->snap.header.h == 480) {
        drm_compose_landscape((uint32_t *)s->argb);
    }
#endif
    if (pthread_create(&thread, NULL, screenshot_thread, s) != 0) {
        LV_LOG_ERROR("Screenshot: no thread");
        shot_free(s);
        return;
    }
    pthread_detach(thread);
}
