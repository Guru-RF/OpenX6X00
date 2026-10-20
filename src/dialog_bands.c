/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Bands app
 *
 *  The band overview of Field Ops (field/bands.c) in the look of the other apps (app_frame):
 *  two columns of bands with their activity bar, cluster and RBN spot counts and who heard
 *  you; the MFK chooses a band, a press tunes a typical frequency of it. Test beacon sends
 *  TEST DE <call> in CW so the skimmers show who hears you.
 */

#include "dialog_bands.h"

#include "app_frame.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cluster/spots.h"
#include "dialog.h"
#include "dialog_field.h"
#include "events.h"
#include "field/bands.h"
#include "keyboard.h"
#include "params/params.h"
#include "radio.h"
#include "styles.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define ROWS    6                   /* per column */
#define COL_W   ((APP_FRAME_W - 18) / 2)
#define ROW_H   30
#define BAR_W   104

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void beacon_cb(button_data_t *b);
static void tune_cb(button_data_t *b);

static button_data_t btn_page   = {.type = BTN_TEXT, .label = "(BANDS 1:1)"};
static button_data_t btn_beacon = {.type = BTN_TEXT, .label = "Test\nbeacon", .press = beacon_cb};
static button_data_t btn_tune   = {.type = BTN_TEXT, .label = "Use\nband", .press = tune_cb};

static buttons_page_t btn_page_1 = {.items = {&btn_page, &btn_beacon, &btn_tune}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .key_cb       = key_cb,
};

dialog_t *dialog_bands = &dialog;

static app_frame_t frame;
static lv_timer_t *timer;
static int         sel;
static lv_coord_t  bar_w;

static struct {
    lv_obj_t *cell, *name, *bar, *word, *counts, *heard;
} rows[FIELD_RBN_BANDS];

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_coord_t x, lv_coord_t w) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, w);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);
    lv_label_set_text(l, "");
    return l;
}

static void fill(void) {
    field_bands_t fb;
    char          buf[32];

    field_bands(&fb);
    for (int b = 0; b < FIELD_RBN_BANDS; b++) {
        int      lv    = fb.level[b];
        uint32_t color = lv >= 4 ? theme_hex(TC_ok) : lv >= 2 ? theme_hex(TC_warn) : lv ? theme_hex(TC_err) : theme_hex(TC_text_faint);
        bool     on    = b == sel;

        lv_obj_set_style_bg_opa(rows[b].cell, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(rows[b].cell, on ? TC(sel_text) : TC(text), 0);
        /* The best band framed */
        lv_obj_set_style_border_width(rows[b].cell, b == fb.best ? 1 : 0, 0);

        lv_obj_set_width(rows[b].bar, 6 + lv * (bar_w - 6) / 6);
        lv_obj_set_style_bg_color(rows[b].bar, lv ? lv_color_hex(color) : TC(track), 0);
        lv_label_set_text(rows[b].word, field_band_word(lv));
        lv_obj_set_style_text_color(rows[b].word, on ? TC(sel_text) : lv_color_hex(color), 0);
        snprintf(buf, sizeof(buf), "%d / %d", fb.spots[b], fb.rbn[b]);
        lv_label_set_text(rows[b].counts, buf);
        if (fb.heard[b])
            snprintf(buf, sizeof(buf), "%d  %+d dB", fb.heard[b], fb.best_snr[b]);
        else
            buf[0] = 0;
        lv_label_set_text(rows[b].heard, buf);
    }

    field_rbn_hit_t hits[5];
    int             n = field_rbn_heard(FIELD_HEARD_S, hits, 5), p;
    char            line[200];

    p = snprintf(line, sizeof(line), "Spots: cluster 30 min / RBN 15 min.   ");
    if (!n)
        p += snprintf(line + p, sizeof(line) - p, "Nobody heard you: Test beacon (CW)");
    else
        p += snprintf(line + p, sizeof(line) - p, "Heard by");
    for (int k = 0; k < n && p < (int)sizeof(line); k++)
        p += snprintf(line + p, sizeof(line) - p, "  %s %+d", hits[k].skimmer, hits[k].snr);
    lv_label_set_text(frame.status, line);
}

static void timer_cb(lv_timer_t *t) {
    static int ticks;

    lv_label_set_text_fmt(frame.head_left, "BANDS   %.4f MHz", cparam_i_get(cfg_fg_freq) / 1e6);
    lv_label_set_text_fmt(frame.head_right, "RBN %s", field_rbn_state());
    if (!t || ++ticks % 5 == 0)
        fill();
}

static void select_band(int b) {
    sel = b < 0 ? 0 : b >= FIELD_RBN_BANDS ? FIELD_RBN_BANDS - 1 : b;
    fill();
}

static void beacon_cb(button_data_t *b) {
    dialog_field_test_beacon();
}

static void tune_cb(button_data_t *b) {
    dialog_field_tune_band(sel);
}

static void cell_click_cb(lv_event_t *e) {
    select_band((int)(intptr_t)lv_event_get_user_data(e));
    tune_cb(NULL);
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            select_band(sel - 1);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            select_band(sel + 1);
            break;
        case LV_KEY_ENTER:
            tune_cb(NULL);
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

static void click_cb(lv_event_t *e) {
    tune_cb(NULL);
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    app_frame_create(&frame, dialog.obj);

    lv_obj_t *body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* The columns as wide as their longest text: VISION's bigger type ran "HIGH" into "5 / 0" and
     * "160m" into the bar. The bar gives way */
    lv_coord_t name_w  = LV_MAX(62, lv_txt_get_width("160m", 4, &sony_22, 0, LV_TEXT_FLAG_NONE) + 8);
    lv_coord_t word_w  = LV_MAX(50, lv_txt_get_width("HIGH", 4, &sony_18, 0, LV_TEXT_FLAG_NONE) + 10);
    lv_coord_t count_w = LV_MAX(62, lv_txt_get_width("99 / 99", 7, &sony_18, 0, LV_TEXT_FLAG_NONE) + 8);
    lv_coord_t bar_x   = 8 + name_w + 4;
    bar_w              = LV_MIN(BAR_W, COL_W - bar_x - 10 - word_w - 2 - count_w - 4 - 60);   /* 60: room for who heard you */
    lv_coord_t word_x  = bar_x + bar_w + 10, count_x = word_x + word_w + 2, heard_x = count_x + count_w + 4;

    for (int b = 0; b < FIELD_RBN_BANDS; b++) {
        lv_obj_t *c = lv_obj_create(body);

        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, COL_W, ROW_H);
        lv_obj_set_pos(c, 6 + (b / ROWS) * (COL_W + 6), 6 + (b % ROWS) * (ROW_H + 2));
        lv_obj_set_style_radius(c, 4, 0);
        lv_obj_set_style_bg_color(c, TC(sel_bg), 0);
        lv_obj_set_style_border_color(c, TC(ok), 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cell_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)b);
        rows[b].cell = c;

        rows[b].name = label(c, &sony_22, 8, name_w);
        lv_label_set_text(rows[b].name, field_band_name(b));

        lv_obj_t *track = lv_obj_create(c);
        lv_obj_remove_style_all(track);
        lv_obj_set_size(track, bar_w, 8);
        lv_obj_align(track, LV_ALIGN_LEFT_MID, bar_x, 0);
        lv_obj_set_style_radius(track, 4, 0);
        lv_obj_set_style_bg_color(track, TC(track), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        rows[b].bar = lv_obj_create(track);
        lv_obj_remove_style_all(rows[b].bar);
        lv_obj_set_height(rows[b].bar, 8);
        lv_obj_set_style_radius(rows[b].bar, 4, 0);
        lv_obj_set_style_bg_opa(rows[b].bar, LV_OPA_COVER, 0);

        rows[b].word   = label(c, &sony_18, word_x, word_w);
        rows[b].counts = label(c, &sony_18, count_x, count_w);
        rows[b].heard  = label(c, &sony_18, heard_x, COL_W - heard_x - 4);
        lv_obj_set_style_text_color(rows[b].heard, TC(ok), 0);
    }
    /* Columns: band, activity, cluster / RBN spots, heard you (skimmers, best SNR) */

    buttons_load_page(&btn_page_1);
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    if (params.callsign.x[0])
        field_rbn_start(params.callsign.x);
    sel = field_band_of(cparam_i_get(cfg_fg_freq));
    if (sel < 0)
        sel = 0;
    timer_cb(NULL);
    timer = lv_timer_create(timer_cb, 1000, NULL);
}

static void destruct_cb(void) {
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    /* Not on the phone's data while the app is closed; the activity of the last minutes stays */
    field_rbn_stop();
}
