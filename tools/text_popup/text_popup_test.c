/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the text window (src/text_popup.c, #220): the WiFi password, the callsign and the other
 *  one-line texts. The real text_popup.c and the real fonts, put in place by font_select.c's fonts_apply() as at boot,
 *  on the PC build's LVGL: an 800 x 480 display flushed into a picture here.
 *
 *  The keys: the on-screen keyboard's J and j type J and j, and so do a USB keyboard's. Its key events go the radio's
 *  way: read by the real key_input.c (CapsLock's hyper key, the keys with no character), made characters by xkb with
 *  the radio's layout (lv_drv_conf.h), then LVGL's keypad, the window's own key handler and the text box.
 *
 *  The picture: for each font a theme or Settings can put there, and VISION's sizes, the text box's line and the
 *  title's whole inside the window's row, and each letter of "I J i j g p q y" drawn whole: its ink down to the last
 *  row the font has for it. Up to v0.3.9 the row was 35 px high whatever the font, as the stock font's line (33 px)
 *  needs, and the other fonts have a higher one (DejaVu's, the default, 43 px): what goes under the baseline was cut
 *  off, so J showed as I, j as i, and g p q y without their tails.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE     /* memfd_create */

#include "lvgl/lvgl.h"
#include "cw_encoder.h"
#include "cw_field.h"
#include "fonts/font_select.h"
#include "gui_styles.h"
#include "input_caps.h"
#include "key_input.h"
#include "paddle_keyer.h"
#include "pubsub_ids.h"
#include "radio.h"
#include "screen_light.h"
#include "screen_main.h"
#include "text_popup.h"
#include "theme.h"

#include <glob.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define W       800
#define H       480
#define TEXT    0xffffff    /* the text, on the black of the display */
#define LETTERS "I J i j g p q y"

/* ---- What text_popup.c calls, stood in for ---- */

lv_color_t bg_color;
lv_style_t msg_style, dialog_item_focus_style;

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

/* ---- What key_input.c calls, stood in for: the USB keyboard a file of its key events ---- */

static int  keys_fd = -1;   /* the key events, as the kernel queues them for the keyboard's node */
static char keys_node[32];  /* that node: /proc/self/fd/N, opened as key_input.c opens /dev/input's */
static bool plugged;

/* /dev/input's keyboards: this one while plugged, and none of the PC's own */
int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *found) {
    static char *paths[] = {keys_node, NULL};

    (void)flags;
    (void)errfunc;
    if (!plugged || strcmp(pattern, "/dev/input/by-path/*-kbd")) {
        return GLOB_NOMATCH;
    }
    found->gl_pathc = 1;
    found->gl_pathv = paths;
    return 0;
}

void globfree(glob_t *found) {
    (void)found;
}

/* sysfs: every key there, the letters too */
bool input_caps_bits(dev_t dev, const char *what, unsigned long *bits, size_t n) {
    (void)dev;
    (void)what;
    memset(bits, 0xff, n * sizeof(*bits));
    return true;
}

/* Not in CW: [ and ] type, no paddle */
bool paddle_keyer_key(int code) {
    (void)code;
    return false;
}

void paddle_keyer_open(const char *path) {
    (void)path;
}

void paddle_keyer_scan(void) {
}

cw_encoder_state_t cw_encoder_state() {
    return CW_ENCODER_IDLE;
}

void cw_encoder_stop() {
}

bool cw_field_key(uint32_t key) {
    (void)key;
    return false;
}

/* The screen lit, no screensaver: every key types */
bool backlight_tick() {
    return false;
}

bool backlight_saver_on() {
    return false;
}

/* The text window over the main screen */
bool main_screen_has_keys(void) {
    return false;
}

void main_screen_chord(int action) {
    (void)action;
}

void main_screen_keyboard_gone(void) {
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

/* A USB keyboard plugged in (true) or out, as udev tells key_input.c */
static void plug(bool in) {
    plugged = in;
    lv_msg_send(MSG_USB_DEVICE_CHANGED, NULL);
}

/* One key event, as evdev sends it: the key, then SYN_REPORT */
static void key_event(uint16_t code, int32_t value) {
    struct input_event ev[] = {{.type = EV_KEY, .code = code, .value = value}, {.type = EV_SYN, .code = SYN_REPORT}};

    if (write(keys_fd, ev, sizeof(ev)) != sizeof(ev)) {
        perror("the key events");
        exit(1);
    }
}

static void tap(uint16_t code) {
    key_event(code, 1);
    key_event(code, 0);
}

/* LVGL's read of the keyboard, as its timer does it: every event waiting */
static void read_keyboard(void) {
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (i->driver->type == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_read_timer_cb(i->driver->read_timer);
        }
    }
}

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

    /* A USB keyboard: no on-screen one, the text box focused. Shift+J, J, then J with Caps Lock on (CapsLock tapped
     * alone, kbd_hyper.c) */
    keys_fd = memfd_create("keyboard", 0);
    if (keys_fd < 0) {
        perror("the keyboard");
        exit(1);
    }
    snprintf(keys_node, sizeof(keys_node), "/proc/self/fd/%d", keys_fd);
    plug(true);
    if (!keyboard_ready()) {
        check(false, "USB keyboard: not opened");
        plug(false);
        return;
    }
    textarea_window_open(ok_cb, NULL);
    key_event(KEY_LEFTSHIFT, 1);
    tap(KEY_J);
    key_event(KEY_LEFTSHIFT, 0);
    tap(KEY_J);
    tap(KEY_CAPSLOCK);
    tap(KEY_J);
    tap(KEY_CAPSLOCK);
    read_keyboard();
    check(!strcmp(textarea_window_get(), "JjJ"), "USB keyboard: Shift+J, J, CapsLock and J type \"%s\", want \"JjJ\"",
          textarea_window_get());
    textarea_window_close();
    plug(false);
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
    keyboard_init();    /* no keyboard yet: as #220's, the on-screen one under the text */

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
