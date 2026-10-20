/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The styles shared by the whole GUI, built from the theme (theme.c), and the geometry of the
 * main screen that comes with them */

#ifndef X6100_STYLES_H
#define X6100_STYLES_H

#include "lvgl/lvgl.h"
#include "params/params.h"
#include "theme.h"
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bars at the top. styles_init() sets the sizes from the theme: VISION has a lower top bar,
 * a taller frequency strip and the S-meter on its own row under it, so that the apps start at
 * the same height in every theme */
extern int top_bar_h, freq_strip_h, clock_w, meter_row_h;

#define TOP_BAR_STD_H       66
#define FREQ_STRIP_STD_H    50
#define TOP_BAR_HEIGHT      top_bar_h
#define TOP_BAR_MARGIN      2           /* around and between the boxes of the top bar */
#define CLOCK_W             clock_w
#define TOP_BAR_BOX_H       (TOP_BAR_HEIGHT - 2 * TOP_BAR_MARGIN)
#define TOP_BAR_METER_X     (TOP_BAR_MARGIN + 360 + 3)     /* right of the 360 px info box */
#define TOP_BAR_METER_W     (800 - TOP_BAR_METER_X - 3 - CLOCK_W - TOP_BAR_MARGIN)

#define FREQ_STRIP_Y        TOP_BAR_HEIGHT
#define FREQ_STRIP_H        freq_strip_h
#define METER_ROW_H         meter_row_h     /* VISION: the S-meter row under the strip; else 0 */
#define VISION_POWER_W      190             /* VISION: the battery box at the right end of that row (clock.c) */

/* The five buttons at the bottom: fonts_apply() makes them taller for fonts with two-line labels */
#define BUTTONS_H_MIN       54
#define BUTTONS_H_MAX       62
extern int buttons_h;
#define BUTTONS_H           buttons_h

/* App windows fill the space between the frequency strip and the buttons */
#define DIALOG_Y            (TOP_BAR_STD_H + FREQ_STRIP_STD_H + TOP_BAR_MARGIN)   /* VISION adds up the same */
#define DIALOG_H            (480 - BUTTONS_H - TOP_BAR_MARGIN - DIALOG_Y)
#define DIALOG_H_MAX        (480 - BUTTONS_H_MIN - TOP_BAR_MARGIN - DIALOG_Y)    /* static buffers */

extern const uint32_t *wf_palette;      /* the waterfall's 256 colours, from the theme */
extern lv_color_t      bg_color;

/* Screen, spectrum, frequency labels, waterfall */
extern lv_style_t background_style, spectrum_style, freq_style, waterfall_style;

/* Buttons and their encoder marks (active: the control the encoder is on now) */
extern lv_style_t btn_style, btn_active_style, btn_disabled_style;
extern lv_style_t btn_mark_style, btn_mark_assigned_style, btn_mark_active_style;

/* Top bar boxes and the messages */
extern lv_style_t msg_style, msg_tiny_style, clock_style, knobs_style;
extern lv_style_t info_style, info_row_style, info_item_style, meter_style, tx_info_style;

/* The text panel (RTTY, NavTex, SWL) and its title */
extern lv_style_t panel_style, panel_info_style;

/* App windows, the frequency strip, Settings rows and dropdowns */
extern lv_style_t dialog_style, freq_strip_style;
extern lv_style_t dialog_item_style, dialog_item_focus_style, dialog_item_edited_style;
extern lv_style_t dialog_dropdown_list_style, dialog_dropdown_sel_style;

extern lv_style_t cw_tune_style;
extern lv_style_t popup_style;          /* menus and popups over the screen or an app */

extern lv_font_t sony_14, sony_16, sony_18, sony_20, sony_22, sony_24, sony_26, sony_28, sony_30;
extern lv_font_t sony_32, sony_34, sony_36, sony_38, sony_40, sony_42, sony_44, sony_60;

/* All the styles again from the theme loaded with theme_init() */
void styles_init(void);
/* Load a theme by name with its font, then styles_init() */
void styles_load(const char *name);
/* Only the theme's colours, images and shapes (styles_init() ends with it) */
void styles_apply(void);
/* Style a dropdown's list as a popup of the theme */
void styles_dropdown_list(lv_obj_t *list);
/* Build s as a box of the theme: colour, gradient, border, corners, and its image at w x h if any */
void styles_theme_box(lv_style_t *s, theme_color_t top, theme_color_t bottom, theme_color_t border, int radius,
                      theme_image_t img, lv_coord_t w, lv_coord_t h);

#ifdef __cplusplus
}
#endif

#endif
