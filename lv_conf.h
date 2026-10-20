/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - LVGL 8.3 settings
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  Only the settings that differ from LVGL's defaults are here; everything else
 *  takes the default from lvgl/src/lv_conf_internal.h. The full list of options,
 *  with LVGL's own notes, is in lvgl/lv_conf_template.h.
 */

/* clang-format off */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 800x480 screen, 32 bit ARGB; LVGL draws on a transparent screen layer */
#define LV_COLOR_DEPTH 32
#define LV_COLOR_SCREEN_TRANSP 1
#define LV_COLOR_MIX_ROUND_OFS (LV_COLOR_DEPTH == 32 ? 0: 128)

/* NEON blending in the gdyuldin LVGL fork; the PC test build sets it to 0 */
#ifndef LV_DRAW_NEON
#define LV_DRAW_NEON 1
#endif

/* malloc/free and memcpy/memset from the C library instead of LVGL's pool */
#define LV_MEM_CUSTOM 1
#define LV_MEMCPY_MEMSET_STD 1

/* Screen refresh about 30 per second; keys and knobs read every 10 ms */
#define LV_DISP_DEF_REFR_PERIOD 33      /*[ms]*/
#define LV_INDEV_DEF_READ_PERIOD 10     /*[ms]*/

/* Layer buffers big enough for a whole 800x480 transparent window strip */
#define LV_LAYER_SIMPLE_BUF_SIZE          (768 * 1024)
#define LV_LAYER_SIMPLE_FALLBACK_BUF_SIZE (128 * 1024)

/* Gradients: up to 5 colour stops, 20 KB cache */
#define LV_GRADIENT_MAX_STOPS 5
#define LV_GRAD_CACHE_DEF_SIZE 20*1024

/* Rotation buffer: 50 lines of 800 pixels, 4 bytes each */
#define LV_DISP_ROT_MAX_BUF (4*800*50)

/* LVGL warnings go to the GUI log through printf, without trace messages */
#define LV_USE_LOG 1
#if LV_USE_LOG
    #define LV_LOG_PRINTF 1
    #define LV_LOG_TRACE_MEM        0
    #define LV_LOG_TRACE_TIMER      0
    #define LV_LOG_TRACE_INDEV      0
    #define LV_LOG_TRACE_DISP_REFR  0
    #define LV_LOG_TRACE_EVENT      0
    #define LV_LOG_TRACE_OBJ_CREATE 0
    #define LV_LOG_TRACE_LAYOUT     0
    #define LV_LOG_TRACE_ANIM       0
#endif

/*Areas to redraw kept per frame (LVGL: 32). Past it the whole screen is redrawn: each VFO step moves
 *the VFO line, the filter band, the grid and band plan marks and the scale labels, old and new place
 *each, and went over 32, so tuning redrew all 800x480 at every step*/
#define LV_INV_BUF_SIZE 128

/* lv_snprintf prints %f */
#define LV_SPRINTF_USE_FLOAT 1

/* Built-in Montserrat sizes used next to our own fonts */
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_22 1

/* Files (images, SSTV pictures) are opened as "A:/path" */
#define LV_USE_FS_POSIX 1
#if LV_USE_FS_POSIX
    #define LV_FS_POSIX_LETTER 'A'
#endif

/* Screenshots and screen recording; message bus between modules */
#define LV_USE_SNAPSHOT 1
#define LV_USE_MSG 1

#define LV_BUILD_EXAMPLES 0

#endif /*LV_CONF_H*/
