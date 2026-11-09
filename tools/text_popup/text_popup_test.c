/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the text window (src/text_popup.c, #220): the WiFi password, the callsign and the other
 *  one-line texts. The real text_popup.c and the real fonts, put in place by font_select.c's fonts_apply() as at boot,
 *  on the PC build's LVGL: an 800 x 480 display flushed into a picture here.
 *
 *  The keys: the on-screen keyboard's J and j type J and j, and so do a USB keyboard's (their keys sent to the text
 *  box as LVGL's keypad sends them, the window's own key handler first).
 *
 *  The picture: for each font a theme or Settings can put there, and VISION's sizes, the text box's line and the
 *  title's whole inside the window's row, and each letter of "I J i j g p q y" drawn whole: its ink down to the last
 *  row the font has for it. Up to v0.3.9 the row was 35 px high whatever the font, as the stock font's line (33 px)
 *  needs, and the other fonts have a higher one (DejaVu's, the default, 43 px): what goes under the baseline was cut
 *  off, so J showed as I, j as i, and g p q y without their tails.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "lvgl/lvgl.h"
#include "fonts/font_select.h"
#include "gui_styles.h"
#include "key_input.h"
#include "radio.h"
#include "text_popup.h"
#include "theme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W       800
#define H       480
#define TEXT    0xffffff    /* the text, on the black of the display */
#define LETTERS "I J i j g p q y"

/* ---- What text_popup.c calls, stood in for ---- */

lv_group_t *keyboard_group;
lv_color_t  bg_color;
lv_style_t  msg_style, dialog_item_focus_style;

static bool usb_keyboard;

bool keyboard_ready() {
    return usb_keyboard;
}

uint16_t radio_change_vol(int16_t df) {
    (void)df;
    return 0;
}

uint32_t theme_hex(theme_color_t c) {
    return c == TC_text ? TEXT : 0x000000;
}

/* LVGL's clock: still, the test draws with lv_refr_now(). With both defined here the library's lv_hal_tick.o is not
 * linked */
uint32_t lv_tick_get(void) {
    return 1000;
}

uint32_t lv_tick_elaps(uint32_t prev_tick) {
    return 1000 - prev_tick;
}

/* gui_styles.c's message box: the window's size and place */
static void styles(void) {
    lv_style_init(&dialog_item_focus_style);
    lv_style_init(&msg_style);
    lv_style_set_text_font(&msg_style, &sony_38);
    lv_style_set_width(&msg_style, 603);
    lv_style_set_x(&msg_style, W / 2 - 603 / 2);
    lv_style_set_y(&msg_style, 270);
    lv_style_set_pad_hor(&msg_style, 10);
    lv_style_set_pad_ver(&msg_style, 20);
    lv_style_set_bg_img_opa(&msg_style, LV_OPA_COVER);
}

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ---- The display ---- */

static uint32_t panel[W * H];

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    for (int y = a->y1; y <= a->y2; y++) {
        for (int x = a->x1; x <= a->x2; x++) {
            panel[y * W + x] = (px++)->full & 0xffffff;
        }
    }
    lv_disp_flush_ready(d);
}

static void display(void) {
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t      drv;
    lv_color_t               *px = malloc(W * H * sizeof(lv_color_t));

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, px, NULL, W * H);
    lv_disp_drv_init(&drv);
    drv.hor_res  = W;
    drv.ver_res  = H;
    drv.flush_cb = flush;
    drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&drv);
    lv_disp_set_bg_color(lv_disp_get_default(), lv_color_black());
    lv_disp_set_bg_opa(lv_disp_get_default(), LV_OPA_COVER);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_TRANSP, 0);
}

static bool ok_cb(void) {
    return true;
}

/* ---- The keys ---- */

/* The on-screen keyboard's key with this text pressed, as a touch or the knob's press does it: LVGL's keyboard types
 * the text of the key selected when its value changes */
static bool press(lv_obj_t *kbd, const char *txt) {
    uint16_t n = ((lv_btnmatrix_t *)kbd)->btn_cnt;

    for (uint16_t i = 0; i < n; i++) {
        if (!strcmp(lv_btnmatrix_get_btn_text(kbd, i), txt)) {
            lv_btnmatrix_set_selected_btn(kbd, i);
            lv_event_send(kbd, LV_EVENT_VALUE_CHANGED, &i);
            return true;
        }
    }
    return false;
}

static void keys(void) {
    lv_obj_t *kbd;

    fonts_apply(FONT_DEJAVU);

    /* No USB keyboard: the on-screen one, focused, in capitals first */
    usb_keyboard = false;
    textarea_window_open(ok_cb, NULL);
    kbd = lv_group_get_focused(keyboard_group);
    if (!kbd || !lv_obj_check_type(kbd, &lv_keyboard_class)) {
        check(false, "on-screen keyboard: not there, or not focused");
        textarea_window_close();
        return;
    }
    bool pressed = press(kbd, "J") && press(kbd, "abc") && press(kbd, "j");

    check(pressed && !strcmp(textarea_window_get(), "Jj"), "on-screen keyboard: J, abc, j type \"%s\", want \"Jj\"",
          textarea_window_get());
    textarea_window_close();

    /* A USB keyboard: the text box focused, xkb's characters as its keys */
    usb_keyboard = true;
    textarea_window_open(ok_cb, NULL);
    lv_group_send_data(keyboard_group, 'J');
    lv_group_send_data(keyboard_group, 'j');
    check(!strcmp(textarea_window_get(), "Jj"), "USB keyboard: J, j type \"%s\", want \"Jj\"", textarea_window_get());
    textarea_window_close();
}

/* ---- The picture ---- */

/* obj inside each of the objects around it, which clip what is in them to their own area */
static bool shown_whole(lv_obj_t *obj) {
    for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
        if (!lv_obj_has_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE) && !_lv_area_is_in(&obj->coords, &p->coords, 0)) {
            return false;
        }
    }
    return true;
}

/* The lowest row with ink in columns x1 .. x2 from y1 to y2, y1 - 1 with none */
static int lowest_ink(int x1, int x2, int y1, int y2) {
    for (int y = y2; y >= y1; y--) {
        for (int x = x1; x <= x2; x++) {
            if (panel[y * W + x]) {
                return y;
            }
        }
    }
    return y1 - 1;
}

static void drawn(const char *name, uint8_t font, bool vision) {
    fonts_apply(font);
    if (vision) {
        fonts_scale(VISION_FONT_STEPS);
    }
    usb_keyboard = false;   /* as #220's: the on-screen keyboard under it */
    textarea_window_open_w_label(ok_cb, NULL, "Key");
    textarea_window_set(LETTERS);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    lv_obj_t        *field = textarea_window_text();
    lv_obj_t        *label = lv_textarea_get_label(field);
    lv_obj_t        *title = lv_obj_get_child(lv_obj_get_child(lv_obj_get_parent(field), 0), 0);
    const lv_font_t *f     = lv_obj_get_style_text_font(field, LV_PART_MAIN);
    lv_coord_t       row_h = lv_obj_get_height(lv_obj_get_parent(field));
    char             cut[256] = "";

    check(shown_whole(field) && shown_whole(title), "%s: the text's line (%d px) and the title's in the row (%d px)",
          name, lv_font_get_line_height(f), row_h);

    for (uint32_t i = 0; LETTERS[i]; i++) {
        lv_font_glyph_dsc_t g;
        lv_point_t          pos;

        if (LETTERS[i] == ' ' || !lv_font_get_glyph_dsc(f, &g, LETTERS[i], 0)) {
            continue;
        }
        lv_label_get_letter_pos(label, i, &pos);

        /* where LVGL draws the glyph's box (lv_draw_sw_letter.c), and how low the picture has ink in it */
        int x1     = label->coords.x1 + pos.x + g.ofs_x;
        int bottom = label->coords.y1 + pos.y + (f->line_height - f->base_line) - g.ofs_y - 1;
        int low    = lowest_ink(x1, x1 + g.box_w - 1, label->coords.y1, bottom + 8);

        if (low != bottom) {
            snprintf(cut + strlen(cut), sizeof(cut) - strlen(cut), " %c (%d of %d px under the baseline)", LETTERS[i],
                     low - (bottom + g.ofs_y), -g.ofs_y);
        }
    }
    check(!cut[0], "%s: \"%s\" drawn whole%s%s", name, LETTERS, cut[0] ? ", cut:" : "", cut);
    textarea_window_close();
}

int main(void) {
    display();
    styles();
    keyboard_group = lv_group_create();

    keys();

    drawn("the stock font", FONT_SONY, false);
    drawn("Roboto", FONT_ROBOTO, false);
    drawn("DejaVu (the default)", FONT_DEJAVU, false);
    drawn("Classic's", FONT_ARIAL, false);
    drawn("the deck themes' Roboto Condensed", FONT_RCOND, false);
    drawn("1bit's Terminus", FONT_PIXEL, false);
    drawn("Codec's Share Tech Mono", FONT_CODEC, false);
    drawn("DejaVu, VISION's sizes", FONT_DEJAVU, true);
    drawn("the stock font, VISION's sizes", FONT_SONY, true);

    printf("%d of %d cases ok\n", good, cases);
    return good == cases ? 0 : 1;
}
