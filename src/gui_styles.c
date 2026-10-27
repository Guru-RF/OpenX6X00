/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#include "gui_styles.h"
#include "theme.h"
#include "mono.h"
#include "fonts/font_select.h"

/* The panel's background pictures: copied to RAM at boot (x6100_daemon) */
#define SHM_IMAGES "A:/dev/shm/images/"

const uint32_t *wf_palette;
lv_color_t      bg_color;

lv_style_t background_style, spectrum_style, freq_style, waterfall_style;
lv_style_t btn_style, btn_active_style, btn_disabled_style;
lv_style_t btn_mark_style, btn_mark_assigned_style, btn_mark_active_style;
lv_style_t msg_style, msg_tiny_style, clock_style, knobs_style;
lv_style_t info_style, info_row_style, info_item_style, meter_style, tx_info_style;
lv_style_t panel_style, panel_info_style;
lv_style_t dialog_style, freq_strip_style;
lv_style_t dialog_item_style, dialog_item_focus_style, dialog_item_edited_style;
lv_style_t dialog_dropdown_list_style, dialog_dropdown_sel_style;
lv_style_t cw_tune_style, popup_style;

int top_bar_h = TOP_BAR_STD_H, freq_strip_h = FREQ_STRIP_STD_H, clock_w = 168, meter_row_h = 0;

/* x of a box w wide in the middle of the screen (odd widths round to the right) */
#define CENTRED(w) (400 - (w) / 2)

static void place(lv_style_t *s, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_style_set_x(s, x);
    lv_style_set_y(s, y);
    lv_style_set_width(s, w);
    lv_style_set_height(s, h);
}

/* Fill, gradient (if the theme draws them and the two colours differ), border, corners */
static void box(lv_style_t *s, theme_color_t top, theme_color_t bottom, theme_color_t border, int radius) {
    bool grad = theme_shape.box_gradient && theme_hex(top) != theme_hex(bottom);

    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_bg_color(s, lv_color_hex(theme_hex(top)));
    lv_style_set_bg_grad_color(s, lv_color_hex(theme_hex(bottom)));
    lv_style_set_bg_grad_dir(s, grad ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE);
    lv_style_set_border_width(s, theme_shape.border);
    lv_style_set_border_color(s, lv_color_hex(theme_hex(border)));
    lv_style_set_radius(s, radius);
}

/* The theme's picture of a surface, made at w x h, replaces fill and border; a theme without
 * one keeps the colours */
static void surface_img(lv_style_t *s, theme_image_t img, lv_coord_t w, lv_coord_t h) {
    const lv_img_dsc_t *pic = theme_image(img, w, h);

    if (!pic) {
        lv_style_remove_prop(s, LV_STYLE_BG_IMG_SRC);
        return;
    }
    lv_style_set_bg_img_src(s, pic);
    lv_style_set_bg_opa(s, LV_OPA_TRANSP);
    lv_style_set_border_width(s, 0);
}

/* The sizes of the top bar and the frequency strip in this theme */
static void bar_sizes(void) {
    top_bar_h    = TOP_BAR_STD_H;
    freq_strip_h = FREQ_STRIP_STD_H;
    clock_w      = 168;
    meter_row_h  = 0;
    if (theme_shape.vision) {
        /* 48 + 70 make the 66 + 50 of the others; the buttons as tall as with DejaVu */
        top_bar_h    = 48;
        freq_strip_h = 70;
        clock_w      = 132;
        meter_row_h  = 50;
        buttons_h    = BUTTONS_H_MAX;
    }
    if (theme_shape.deck) {
        /* two rows of 28 px, the scales beside the frequency */
        top_bar_h    = 56;
        freq_strip_h = 102;
        clock_w      = 280;
    }
}

static void screen_styles(void) {
    lv_style_set_bg_opa(&spectrum_style, LV_OPA_COVER);
    lv_style_set_border_color(&spectrum_style, lv_color_hex(0xAAAAAA));
    lv_style_set_border_width(&spectrum_style, 0);
    lv_style_set_radius(&spectrum_style, 0);
    lv_style_set_width(&spectrum_style, 800);
    lv_style_set_x(&spectrum_style, 0);

    /* the frequencies under the spectrum; VISION's font is the 28 of fonts_scale() */
    lv_style_set_text_font(&freq_style, &sony_22);
    lv_style_set_pad_ver(&freq_style, theme_shape.vision ? 5 : 9);
    lv_style_set_width(&freq_style, 150);
    lv_style_set_height(&freq_style, 36);
    lv_style_set_text_align(&freq_style, LV_TEXT_ALIGN_CENTER);

    lv_style_set_border_color(&waterfall_style, lv_color_hex(0xAAAAAA));
    lv_style_set_border_width(&waterfall_style, 0);
    lv_style_set_radius(&waterfall_style, 0);
    lv_style_set_clip_corner(&waterfall_style, true);
    lv_style_set_width(&waterfall_style, 800);
    lv_style_set_x(&waterfall_style, 0);
    lv_style_set_pad_all(&waterfall_style, 0);
}

static void button_styles(void) {
    lv_style_set_text_font(&btn_style, &sony_30);

    /* the bar on the left edge of an encoder button (styles_apply() sizes it) */
    lv_style_set_radius(&btn_mark_style, 2);
    lv_style_set_bg_opa(&btn_mark_style, LV_OPA_COVER);
    lv_style_set_outline_width(&btn_mark_style, 0);
    lv_style_set_border_width(&btn_mark_style, 0);
}

/* The two messages (msg.c, msg_tiny.c) and the text panel */
static void message_styles(void) {
    lv_style_set_text_font(&msg_style, &sony_38);
    lv_style_set_width(&msg_style, 603);
    lv_style_set_x(&msg_style, CENTRED(603));
    lv_style_set_y(&msg_style, 270);
    lv_style_set_pad_hor(&msg_style, 10);
    lv_style_set_pad_ver(&msg_style, 20);
    lv_style_set_bg_img_opa(&msg_style, LV_OPA_COVER);

    /* the big one in the middle, centred on the waterfall's first 36 px */
    lv_style_set_text_font(&msg_tiny_style, &sony_60);
    place(&msg_tiny_style, CENTRED(324), 160 - 66 / 2 + 36 / 2, 324, 66);
    lv_style_set_pad_ver(&msg_tiny_style, 12);

    /* RTTY, NavTex and the SWL panel: 24 px text */
    lv_style_set_text_font(&panel_style, &sony_24);
    place(&panel_style, CENTRED(795), 230, 795, 480 - BUTTONS_H - TOP_BAR_MARGIN - 230);
    lv_style_set_pad_ver(&panel_style, 10);
    lv_style_set_pad_hor(&panel_style, 10);
    lv_style_set_radius(&panel_style, 0);
    lv_style_set_bg_img_opa(&panel_style, LV_OPA_COVER);

    lv_style_set_align(&panel_info_style, LV_ALIGN_OUT_TOP_LEFT);
    lv_style_set_y(&panel_info_style, -38);
    lv_style_set_text_font(&panel_info_style, &sony_30);
    if (theme_shape.vision) {
        /* in the panel's top right corner over its text (panel.cpp): above the panel it covered the scale */
        lv_style_set_align(&panel_info_style, LV_ALIGN_TOP_RIGHT);
        lv_style_set_y(&panel_info_style, -lv_font_get_line_height(&sony_24));
        lv_style_set_text_font(&panel_info_style, &sony_24);
    }
}

/* App windows, the frequency strip, the rows and dropdowns of Settings */
static void window_styles(void) {
    lv_style_set_text_font(&dialog_style, &sony_36);
    place(&dialog_style, CENTRED(796), DIALOG_Y, 796, DIALOG_H);
    lv_style_set_pad_ver(&dialog_style, 0);
    lv_style_set_pad_hor(&dialog_style, 0);

    place(&freq_strip_style, TOP_BAR_MARGIN, FREQ_STRIP_Y, 800 - 2 * TOP_BAR_MARGIN, FREQ_STRIP_H);

    lv_style_set_bg_opa(&dialog_item_style, LV_OPA_TRANSP);
    lv_style_set_bg_opa(&dialog_item_edited_style, LV_OPA_COVER);
    lv_style_set_text_font(&dialog_dropdown_list_style, &sony_30);
}

/* Info, S-meter (TX info over it) and clock; the knob info and the CW tuning box */
static void top_bar_styles(void) {
    lv_style_set_align(&clock_style, LV_ALIGN_TOP_RIGHT);
    place(&clock_style, -TOP_BAR_MARGIN, TOP_BAR_MARGIN, CLOCK_W, TOP_BAR_BOX_H);
    lv_style_set_pad_all(&clock_style, 0);

    lv_style_set_align(&info_style, LV_ALIGN_TOP_LEFT);
    place(&info_style, TOP_BAR_MARGIN, TOP_BAR_MARGIN,
          theme_shape.vision ? 800 - CLOCK_W - 3 * TOP_BAR_MARGIN - 3 : 360, TOP_BAR_BOX_H);
    lv_style_set_pad_hor(&info_style, 5);
    lv_style_set_pad_ver(&info_style, 0);
    lv_style_set_pad_row(&info_style, 0);

    lv_style_set_align(&info_row_style, LV_ALIGN_CENTER);
    lv_style_set_radius(&info_row_style, 0);
    lv_style_set_border_width(&info_row_style, 0);
    lv_style_set_bg_opa(&info_row_style, LV_OPA_0);
    lv_style_set_pad_all(&info_row_style, 0);
    lv_style_set_pad_column(&info_row_style, 1);

    lv_style_set_text_font(&info_item_style, theme_shape.vision ? &sony_28 : &sony_20);
    lv_style_set_pad_ver(&info_item_style, 1);
    lv_style_set_radius(&info_item_style, 3);

    /* the S-meter box, and TX info in the same place; VISION: the row under the strip, the
     * S-meter leaving the battery box room at its right end */
    lv_style_t *meters[] = {&meter_style, &tx_info_style};

    for (int i = 0; i < 2; i++) {
        lv_style_set_align(meters[i], LV_ALIGN_TOP_LEFT);
        if (theme_shape.vision) {
            place(meters[i], TOP_BAR_MARGIN, FREQ_STRIP_Y + FREQ_STRIP_H + TOP_BAR_MARGIN,
                  i ? 800 - 2 * TOP_BAR_MARGIN : 800 - 3 * TOP_BAR_MARGIN - VISION_POWER_W,
                  METER_ROW_H - 2 * TOP_BAR_MARGIN);
        } else {
            place(meters[i], TOP_BAR_METER_X, TOP_BAR_MARGIN, TOP_BAR_METER_W, TOP_BAR_BOX_H);
        }
        lv_style_set_pad_all(meters[i], 0);
    }

    lv_style_set_text_font(&knobs_style, &sony_24);
    lv_style_set_radius(&knobs_style, 8);
    lv_style_set_pad_hor(&knobs_style, 5);
    lv_style_set_pad_ver(&knobs_style, 3);

    lv_style_set_radius(&cw_tune_style, 5);
    lv_style_set_x(&cw_tune_style, 30);
    lv_style_set_y(&cw_tune_style, FREQ_STRIP_Y + FREQ_STRIP_H + METER_ROW_H + 18);   /* VISION: under the S-meter */
}

/* Deck (Black Metal, Total Black): rows and panel the full width, the clock drawn on the rows,
 * the S-meter and TX info on the frequency panel */
static void deck_layout(void) {
    place(&info_style, 0, 0, 800, TOP_BAR_HEIGHT);
    lv_style_set_pad_hor(&info_style, 0);
    lv_style_set_x(&clock_style, -10);
    lv_style_set_y(&clock_style, 0);
    lv_style_set_height(&clock_style, TOP_BAR_HEIGHT - 1);
    place(&meter_style, 0, FREQ_STRIP_Y, 300, FREQ_STRIP_H - 1);
    place(&tx_info_style, 0, FREQ_STRIP_Y, 300, FREQ_STRIP_H - 1);
    lv_style_set_x(&freq_strip_style, 0);
    lv_style_set_width(&freq_strip_style, 800);
}

/* Again on a theme change: reset, not init, so that the old theme's properties go */
void styles_init(void) {
    lv_style_t *all[] = {
        &background_style, &spectrum_style, &freq_style, &waterfall_style,
        &btn_style, &btn_active_style, &btn_disabled_style, &btn_mark_style, &btn_mark_assigned_style,
        &btn_mark_active_style, &msg_style, &msg_tiny_style, &panel_style, &panel_info_style,
        &dialog_style, &freq_strip_style, &dialog_item_style, &dialog_item_focus_style,
        &dialog_item_edited_style, &dialog_dropdown_list_style, &dialog_dropdown_sel_style,
        &clock_style, &knobs_style, &info_style, &info_row_style, &info_item_style,
        &meter_style, &tx_info_style, &popup_style, &cw_tune_style,
    };

    bar_sizes();
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        lv_style_reset(all[i]);
    }
    screen_styles();
    button_styles();
    message_styles();
    window_styles();
    top_bar_styles();
    if (theme_shape.deck) {
        deck_layout();
    }
    styles_apply();
}

void styles_load(const char *name) {
    theme_init(name);    /* an unknown name gives RF.Guru */
    fonts_apply(theme_shape.font >= 0 ? theme_shape.font : params.font.x);
    if (theme_shape.vision) {
        fonts_scale(VISION_FONT_STEPS);   /* apps, Settings and popups bigger too */
    }
    mono_set(theme_shape.mono);   /* 1bit: ink and paper only; the others: LVGL's drawing as it is */
    styles_init();
}

/* The messages and the panel as the theme's popups; with background pictures (Legacy) the panel
 * shows the picture instead */
static void message_colours(bool strong) {
    lv_style_t *boxes[] = {&msg_style, &msg_tiny_style, &panel_style};

    for (int i = 0; i < 3; i++) {
        lv_style_t *s = boxes[i];

        if (s == &panel_style && theme_shape.images) {
            lv_style_remove_prop(s, LV_STYLE_BG_OPA);
            lv_style_remove_prop(s, LV_STYLE_BG_COLOR);
            lv_style_remove_prop(s, LV_STYLE_BORDER_WIDTH);
            lv_style_remove_prop(s, LV_STYLE_BORDER_COLOR);
            lv_style_set_bg_img_src(s, theme_shape.images == 1 ? SHM_IMAGES "panel_dark.bin" : SHM_IMAGES "panel.bin");
            continue;
        }
        lv_style_remove_prop(s, LV_STYLE_BG_IMG_SRC);
        lv_style_set_bg_opa(s, LV_OPA_COVER);
        lv_style_set_bg_color(s, TC(popup));
        lv_style_set_border_width(s, strong ? 3 : theme_shape.border);
        lv_style_set_border_color(s, TC(popup_border));
        if (s != &panel_style) {
            lv_style_set_radius(s, theme_shape.radius_popup);
        }
    }
}

static void button_colours(void) {
    lv_grad_dir_t grad     = theme_shape.btn_gradient ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE;
    bool          top_line = theme_shape.rule || theme_shape.deck;   /* Holo, deck: a line on top marks the active one */

    lv_style_set_bg_color(&btn_style, TC(btn));
    lv_style_set_bg_grad_color(&btn_style, TC(btn_grad));
    lv_style_set_bg_grad_dir(&btn_style, grad);
    lv_style_set_border_width(&btn_style, theme_shape.border);
    lv_style_set_border_color(&btn_style, TC(btn_border));
    lv_style_set_text_color(&btn_style, TC(btn_text));
    lv_style_set_radius(&btn_style, theme_shape.radius_btn);

    lv_style_set_bg_color(&btn_active_style, TC(btn_active));
    lv_style_set_bg_grad_color(&btn_active_style, TC(btn_active_grad));
    lv_style_set_bg_grad_dir(&btn_active_style, grad);
    lv_style_set_border_color(&btn_active_style, TC(btn_active_border));
    lv_style_set_border_width(&btn_active_style, top_line ? 3 : LV_MAX(theme_shape.border, 2));
    lv_style_set_border_side(&btn_active_style, top_line ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_FULL);
    lv_style_set_border_side(&btn_style, theme_shape.deck ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_FULL);
    lv_style_set_text_color(&btn_active_style, TC(btn_active_text));

    /* disabled: the same solid button, text and border dimmed */
    lv_style_set_opa(&btn_disabled_style, LV_OPA_COVER);
    lv_style_set_text_color(&btn_disabled_style, TC(btn_off_text));
    lv_style_set_border_color(&btn_disabled_style, TC(btn_off_border));
    if (theme_shape.mono) {     /* 1bit: dimmed is stippled text and a dotted frame (mono.c), no grey */
        lv_style_set_text_opa(&btn_disabled_style, LV_OPA_30);
        lv_style_set_border_opa(&btn_disabled_style, LV_OPA_30);
    }

    lv_style_set_bg_color(&btn_mark_style, TC(btn_mark));
    lv_style_set_opa(&btn_mark_assigned_style, theme_shape.focus_solid ? LV_OPA_COVER : LV_OPA_40);
    lv_style_set_bg_color(&btn_mark_assigned_style, TC(btn_mark_assigned));
    lv_style_set_opa(&btn_mark_active_style, LV_OPA_COVER);
    lv_style_set_bg_color(&btn_mark_active_style, TC(btn_mark_active));
}

/* Settings rows: the focused one and the one being changed */
static void focus_colours(bool strong) {
    lv_style_set_text_color(&dialog_item_focus_style, TC(focus_text));
    lv_style_set_text_color(&dialog_item_edited_style, TC(focus_text));
    if (theme_shape.focus_solid) {
        lv_style_set_bg_opa(&dialog_item_focus_style, LV_OPA_COVER);
        lv_style_set_bg_color(&dialog_item_focus_style, TC(focus));
        lv_style_set_border_width(&dialog_item_focus_style, strong ? 3 : 2);
        lv_style_set_border_color(&dialog_item_focus_style, TC(sel_border));
        lv_style_set_bg_color(&dialog_item_edited_style, TC(edited));
    } else {
        lv_style_set_bg_opa(&dialog_item_focus_style, 128);
        lv_style_remove_prop(&dialog_item_focus_style, LV_STYLE_BG_COLOR);
        lv_style_set_border_width(&dialog_item_focus_style, 2);
        lv_style_set_border_color(&dialog_item_focus_style, TC(focus));
        lv_style_remove_prop(&dialog_item_edited_style, LV_STYLE_BG_COLOR);
    }
    if (theme_shape.mono) {     /* 1bit: the focused row inverted, the one being changed framed */
        lv_style_set_bg_opa(&dialog_item_edited_style, LV_OPA_TRANSP);
        lv_style_set_text_color(&dialog_item_edited_style, TC(text));
        lv_style_set_border_width(&dialog_item_edited_style, 2);
        lv_style_set_border_color(&dialog_item_edited_style, TC(sel_border));
    }
}

/* Popups, menus and dropdown lists: the theme's popup colours, border and corners */
static void popup_colours(bool strong, lv_color_t text) {
    lv_style_t *pops[] = {&popup_style, &dialog_dropdown_list_style};

    for (int i = 0; i < 2; i++) {
        lv_style_set_bg_color(pops[i], TC(popup));
        lv_style_set_bg_opa(pops[i], LV_OPA_COVER);
        lv_style_set_border_color(pops[i], TC(popup_border));
        lv_style_set_border_width(pops[i], strong ? 3 : theme_shape.border);
        lv_style_set_radius(pops[i], theme_shape.radius_popup);
    }
    lv_style_set_text_color(&dialog_dropdown_list_style, text);
    lv_style_set_bg_color(&dialog_dropdown_sel_style, TC(sel_bg));
    lv_style_set_bg_opa(&dialog_dropdown_sel_style, LV_OPA_COVER);
    lv_style_set_text_color(&dialog_dropdown_sel_style, TC(sel_text));
}

/* The theme's pictures at the size of each surface (buttons 160 px wide, inset by the gap) */
static void surface_images(void) {
    int        inset = 1 + theme_shape.btn_gap;
    lv_coord_t bw = 160 - 2 * inset, bh = BUTTONS_H - 2 * inset;

    lv_style_set_bg_opa(&btn_style, LV_OPA_COVER);
    lv_style_remove_prop(&btn_active_style, LV_STYLE_BG_OPA);
    lv_style_remove_prop(&btn_disabled_style, LV_STYLE_BG_OPA);
    lv_style_remove_prop(&btn_disabled_style, LV_STYLE_BORDER_WIDTH);

    surface_img(&background_style, TI_bg, 800, 480);
    lv_style_set_bg_opa(&background_style, LV_OPA_COVER);   /* the colour stays under a picture */
    surface_img(&btn_style, TI_btn, bw, bh);
    surface_img(&btn_active_style, TI_btn_active, bw, bh);
    surface_img(&btn_disabled_style, TI_btn_off, bw, bh);
    surface_img(&info_style, TI_bar, 360, TOP_BAR_BOX_H);
    surface_img(&meter_style, TI_bar, TOP_BAR_METER_W, TOP_BAR_BOX_H);
    surface_img(&tx_info_style, TI_bar, TOP_BAR_METER_W, TOP_BAR_BOX_H);
    surface_img(&clock_style, TI_bar, CLOCK_W, TOP_BAR_BOX_H);
    surface_img(&freq_strip_style, TI_strip, 800 - 2 * TOP_BAR_MARGIN, FREQ_STRIP_H);
    surface_img(&dialog_style, TI_dialog, 796, DIALOG_H);

    /* encoder marks: the theme's LED picture, else a bar (deck: a 3 px line along the top) */
    if (theme_shape.mark[1] > 0) {
        lv_style_set_width(&btn_mark_style, theme_shape.mark[1]);
        lv_style_set_height(&btn_mark_style, theme_shape.mark[2]);
        surface_img(&btn_mark_style, TI_mark, theme_shape.mark[1], theme_shape.mark[2]);
        surface_img(&btn_mark_active_style, TI_mark_active, theme_shape.mark[1], theme_shape.mark[2]);
    } else {
        lv_style_remove_prop(&btn_mark_active_style, LV_STYLE_BG_IMG_SRC);
        lv_style_remove_prop(&btn_mark_active_style, LV_STYLE_BG_OPA);
        lv_style_set_width(&btn_mark_style, theme_shape.deck ? bw : 4);
        lv_style_set_height(&btn_mark_style, theme_shape.deck ? 3 : BUTTONS_H - 2 - 20);
        lv_style_set_bg_opa(&btn_mark_style, LV_OPA_COVER);
        lv_style_remove_prop(&btn_mark_style, LV_STYLE_BG_IMG_SRC);
        if (theme_shape.mono) lv_style_set_width(&btn_mark_active_style, 8);   /* 1bit: the knob's one twice as wide */
    }
}

void styles_apply(void) {
    lv_style_t *bar[]  = {&clock_style, &info_style, &meter_style, &tx_info_style};
    lv_style_t *text[] = {&freq_style, &dialog_style, &dialog_item_style, &clock_style,
                          &knobs_style, &msg_style, &msg_tiny_style};
    bool        strong = theme_shape.border >= 3;      /* high contrast: thick borders, solid boxes */
    lv_color_t  fg     = TC(text);

    wf_palette = theme_waterfall();
    bg_color   = TC(bg);
    lv_style_set_bg_color(&background_style, bg_color);

    for (int i = 0; i < 4; i++) {
        box(bar[i], TC_bar, TC_bar_grad, TC_bar_border, theme_shape.radius);
    }
    box(&dialog_style, TC_dialog, TC_dialog_grad, TC_dialog_border, theme_shape.radius);
    box(&freq_strip_style, TC_strip, TC_strip_grad, TC_strip_border, theme_shape.radius);

    /* deck: only a line under the rows and under the strip; clock and S-meter drawn on them */
    lv_style_set_border_side(&info_style, theme_shape.deck ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_FULL);
    lv_style_set_border_side(&freq_strip_style, theme_shape.deck ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_FULL);
    if (theme_shape.deck) {
        lv_style_set_bg_opa(&clock_style, LV_OPA_TRANSP);
        lv_style_set_border_width(&clock_style, 0);
        lv_style_set_bg_opa(&meter_style, LV_OPA_TRANSP);
        lv_style_set_border_width(&meter_style, 0);
    }

    message_colours(strong);
    button_colours();

    lv_style_set_text_color(&panel_info_style, TC(text_mute));
    lv_style_set_blend_mode(&panel_info_style, theme_shape.images ? LV_BLEND_MODE_ADDITIVE : LV_BLEND_MODE_NORMAL);
    lv_style_set_bg_color(&spectrum_style, TC(panel));
    lv_style_set_bg_color(&waterfall_style, TC(panel));

    for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
        lv_style_set_text_color(text[i], fg);
    }
    /* the background pictures are dark: their panel text stays white */
    lv_style_set_text_color(&panel_style, theme_shape.images ? lv_color_white() : fg);

    focus_colours(strong);

    /* knob info and CW tuning: see-through, or framed in high contrast. The tuning box uses
     * bg_opa, not opa: an object opa is drawn in a layer at every frame of its fade */
    lv_style_t *overlays[] = {&knobs_style, &cw_tune_style};
    lv_opa_t    see[]      = {LV_OPA_60, LV_OPA_50};

    for (int i = 0; i < 2; i++) {
        lv_style_set_bg_color(overlays[i], TC(panel));
        lv_style_set_bg_opa(overlays[i], strong ? LV_OPA_COVER : see[i]);
        lv_style_set_border_width(overlays[i], strong ? 2 : theme_shape.mono ? 1 : 0);   /* 1bit: solid, framed */
        lv_style_set_border_color(overlays[i], TC(panel_border));
    }

    popup_colours(strong, fg);
    surface_images();

    lv_style_set_border_width(&info_item_style, theme_shape.pill_border);
    lv_style_set_border_color(&info_item_style, TC(pill_border));

    /* gradients are cached by size, not colour: drop the old theme's */
    lv_gradient_set_cache_size(LV_GRAD_CACHE_DEF_SIZE);
    lv_obj_invalidate(lv_scr_act());
}

void styles_dropdown_list(lv_obj_t *list) {
    lv_obj_add_style(list, &dialog_dropdown_list_style, 0);
    lv_obj_add_style(list, &dialog_dropdown_sel_style, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_add_style(list, &dialog_dropdown_sel_style, LV_PART_SELECTED | LV_STATE_PRESSED);
}

/* For screens laid out elsewhere (walkie.c); built again after a theme change (walkie_drop()) */
void styles_theme_box(lv_style_t *s, theme_color_t top, theme_color_t bottom, theme_color_t border, int radius,
                      theme_image_t img, lv_coord_t w, lv_coord_t h) {
    lv_style_reset(s);
    box(s, top, bottom, border, radius);
    surface_img(s, img, w, h);
}
