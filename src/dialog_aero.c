/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  HF Air Band: the HF aeronautical frequencies (aero.c) heard in the area chosen, the VOLMET weather broadcasts first,
 *  then the oceanic and remote air routes and the two common channels; the airlines' worldwide LDOC list on its own.
 *  A row per frequency under its family's name, with the station the broadcast list (EiBi, Settings > Info) has on the
 *  air there now. The MFK chooses, a press tunes it in USB through the radio's own tuning (the band's memory, the
 *  mode's filter), Save keeps it as a channel of the Channels app, Scan hands the same list to the Scanner.
 *
 *  Nothing here keys the radio and nothing here adds a TX rule: PTT on these frequencies follows the radio's own, as
 *  anywhere outside the amateur bands (cfg_tx_refusal): refused ("No TX on 8.891 MHz") unless Settings > TX Cal > TX
 *  outside ham bands is on, and then with the radio's power limit and SWR protection. Transmitting there is the
 *  operator's responsibility under their licence (MANUAL.md says so).
 *
 *  After condoruy's CX8ANM edition (its HF air band app); the lists are the ITU Plan's now, see aero.c.
 */

#include "dialog_aero.h"
#include "voice.h"
#include "voice_focus.h"

#include "aero.h"
#include "app_frame.h"
#include "band_plan.h"
#include "broadcast_db.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "channel_list.h"
#include "dialog_scan.h"
#include "events.h"
#include "freq_strip.h"
#include "keyboard.h"
#include "msg_box.h"
#include "params/params.h"
#include "radio.h"
#include "screen_main.h"
#include "styles.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define MAX_FREQS 512
#define MAX_ROWS  (MAX_FREQS + 64)    /* and a heading for each family */
#define COL0_W   190
#define EIBI_HZ  500        /* a broadcast list entry this close to the carrier is on it */

typedef struct {
    int16_t  fam;
    uint16_t khz;           /* 0: the family's heading */
} row_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void area_cb(button_data_t *b);
static void tune_cb(button_data_t *b);
static void save_cb(button_data_t *b);
static void scan_cb(button_data_t *b);

static button_data_t btn_area = {.type = BTN_TEXT, .label = "Area", .press = area_cb};
static button_data_t btn_tune = {.type = BTN_TEXT, .label = "Tune", .press = tune_cb};
static button_data_t btn_save = {.type = BTN_TEXT, .label = "Save\nchannel", .press = save_cb};
static button_data_t btn_scan = {.type = BTN_TEXT, .label = "Scan", .press = scan_cb};

static buttons_page_t page = {.items = {&btn_area, &btn_tune, &btn_save, &btn_scan}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
};

dialog_t *dialog_aero = &dialog;

static row_t       rows[MAX_ROWS];
static int         rows_n, sel = -1;
static app_frame_t frame;
static ObserverDelayed *freq_obs;    /* the row the radio is on follows the VFO */
static lv_obj_t   *table;

int dialog_aero_area(void) {
    int a = params.aero_area.x;

    return a > 0 && a <= AERO_AREAS ? a - 1 : aero_area_for_region(band_plan_region());
}

/* The station the broadcast list has on the air at khz now, NULL none */
static const char *on_air(uint16_t khz, broadcast_match_t *m) {
    return broadcast_db_find_matches(khz * 1000, EIBI_HZ, m, 1) ? m->station : NULL;
}

static bool is_current(int r) {
    return rows[r].khz && rows[r].khz * 1000 == cparam_i_get(cfg_fg_freq);
}

static void show_head(void) {
    lv_label_set_text_fmt(frame.head_left, "HF AIR BAND   #%06x %s#", theme_hex(TC_accent),
                          _(aero_area_name(dialog_aero_area())));
    lv_label_set_text(frame.head_right, "USB");
}

static void show_status(void) {
    if (sel < 0 || !rows[sel].khz) {
        lv_label_set_text(frame.status, "MFK: choose - press: tune");
        return;
    }
    const aero_family_t *f = aero_family(rows[sel].fam);

    lv_label_set_text_fmt(frame.status, "%s - %s - %u kHz USB", f->code, _(f->name), rows[sel].khz);
}

static void fill(void) {
    static int16_t    fam[MAX_FREQS];
    static uint16_t   khz[MAX_FREQS];
    int               n = aero_rows(dialog_aero_area(), fam, khz, MAX_FREQS);
    broadcast_match_t m;

    rows_n = 0;
    for (int i = 0; i < n && rows_n < MAX_ROWS - 1; i++) {
        if (i == 0 || fam[i] != fam[i - 1]) rows[rows_n++] = (row_t){fam[i], 0};
        rows[rows_n++] = (row_t){fam[i], khz[i]};
    }
    lv_table_set_row_cnt(table, rows_n ? rows_n : 1);
    for (int r = 0; r < rows_n; r++) {
        const aero_family_t *f = aero_family(rows[r].fam);

        lv_table_clear_cell_ctrl(table, r, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
        if (!rows[r].khz) {
            lv_table_set_cell_value_fmt(table, r, 0, "%s   %s", f->code, _(f->name));
            lv_table_add_cell_ctrl(table, r, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT | LV_TABLE_CELL_CTRL_TEXT_CROP);
            lv_table_set_cell_value(table, r, 1, "");
            continue;
        }
        const char *st = on_air(rows[r].khz, &m);

        lv_table_set_cell_value_fmt(table, r, 0, "%u kHz", rows[r].khz);
        lv_table_set_cell_value(table, r, 1, st ? st : "");
    }
}

/* Spoken: "2950 kilohertz, CAR, Caribbean, on the air: New York VOLMET, 3 of 40" */
static void say_sel(const char *first) {
    broadcast_match_t m;
    char              text[240];
    int               pos = 0, total = 0;

    if (sel < 0 || !rows[sel].khz) return;
    for (int r = 0; r < rows_n; r++) {
        if (!rows[r].khz) continue;
        total++;
        if (r <= sel) pos++;
    }
    const aero_family_t *f  = aero_family(rows[sel].fam);
    const char          *st = on_air(rows[sel].khz, &m);

    snprintf(text, sizeof(text), "%s%s%u kilohertz, %s, %s%s%s, %i of %i", first ? first : "", first ? ". " : "",
             rows[sel].khz, f->code, _(f->name), st ? ", on the air: " : "", st ? st : "", pos, total);
    voice_say_clean(text, first == NULL);
}

/* The selected row in the middle of the list, its heading above it in sight */
static void show_sel(void) {
    lv_table_t *t = (lv_table_t *)table;
    lv_coord_t  y = 0;

    if (sel < 0) return;
    for (int i = 0; i < sel; i++) y += t->row_h[i];
    y += t->row_h[sel] / 2 - lv_obj_get_content_height(table) / 2;
    lv_obj_scroll_to_y(table, y > 0 ? y : 0, LV_ANIM_OFF);
    lv_obj_invalidate(table);
    show_status();
}

static void step(int dir) {
    for (int i = sel + dir; i >= 0 && i < rows_n; i += dir)
        if (rows[i].khz) {
            sel = i;
            break;
        }
    show_sel();
    say_sel(NULL);
}

/* The row the radio is on, else the first frequency */
static void select_current(void) {
    sel = -1;
    for (int r = 0; r < rows_n; r++)
        if (rows[r].khz && (sel < 0 || is_current(r))) {
            sel = r;
            if (is_current(r)) break;
        }
    show_sel();
}

static void tune_row(int r) {
    broadcast_match_t m;
    char              label[64];

    if (r < 0 || r >= rows_n || !rows[r].khz) return;

    int32_t              hz = rows[r].khz * 1000;
    const aero_family_t *f  = aero_family(rows[r].fam);

    if (radio_get_state() != RADIO_RX) {
        msg_update_text_fmt("HF Air Band: not while transmitting");
        return;
    }
    /* The frequency first, as the Channels app: a tune into another band loads that band's mode, which USB then
     * replaces */
    main_screen_set_freq(hz);
    if (cparam_i_get(cfg_cur_mode) != radio_mode_usb) cparam_i_set(cfg_cur_mode, radio_mode_usb);

    const char *st = on_air(rows[r].khz, &m);

    snprintf(label, sizeof(label), "%s", st ? st : f->code);
    freq_strip_set_label(label, hz);
    msg_update_text_fmt("%s %u kHz USB", f->code, rows[r].khz);
    lv_obj_invalidate(table);
}

static void area_cb(button_data_t *b) {
    (void)b;
    static const int next[AERO_AREAS] = {
        [AERO_AREA_EUR] = AERO_AREA_AMER, [AERO_AREA_AMER] = AERO_AREA_ASIA, [AERO_AREA_ASIA] = AERO_AREA_LDOC,
        [AERO_AREA_LDOC] = AERO_AREA_ALL, [AERO_AREA_ALL] = AERO_AREA_EUR,
    };

    params_uint8_set(&params.aero_area, (uint8_t)(next[dialog_aero_area()] + 1));
    show_head();
    fill();
    select_current();
}

static void tune_cb(button_data_t *b) {
    (void)b;
    tune_row(sel);
}

static void save_cb(button_data_t *b) {
    (void)b;
    broadcast_match_t m;
    char              name[CHANNEL_NAME_MAX];
    int32_t           f  = cparam_i_get(cfg_fg_freq);
    const char       *st = f % 1000 || f > 30000000 ? NULL : on_air((uint16_t)(f / 1000), &m);   /* HF, whole kHz */

    /* What the radio is on now, named after the row it came from when it is still there */
    if (st) {
        snprintf(name, sizeof(name), "%s", st);
    } else if (sel >= 0 && is_current(sel)) {
        snprintf(name, sizeof(name), "%s %u", aero_family(rows[sel].fam)->code, rows[sel].khz);
    } else {
        name[0] = 0;
    }
    if (channels_add_named(name)) msg_update_text_fmt("Saved channel %s", name);
    else msg_update_text_fmt("Channel not saved");
}

static void scan_async(void *arg) {
    (void)arg;
    dialog_scan_preset(SCAN_SRC_HF_AIR);
    main_screen_start_app(ACTION_APP_SCAN);     /* closes this window first */
}

static void scan_cb(button_data_t *b) {
    (void)b;
    lv_async_call(scan_async, NULL);
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            step(-1);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            step(1);
            break;
        case LV_KEY_ENTER:
            tune_row(sel);
            break;
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            break;
    }
}

/* MFK press: the encoder sends a click, not ENTER */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();

    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) tune_row(sel);
}

/* A tap on a row tunes it */
static void table_click_cb(lv_event_t *e) {
    (void)e;
    uint16_t r, c;

    lv_table_get_selected_cell(table, &r, &c);
    if (r >= rows_n || !rows[r].khz) return;
    sel = r;
    show_sel();
    tune_row(sel);
}

static void draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t r = dsc->id / 2, c = dsc->id % 2;

    if (r >= (uint32_t)rows_n) return;
    dsc->rect_dsc->bg_opa   = LV_OPA_COVER;
    dsc->rect_dsc->bg_color = TC(panel);
    if (!rows[r].khz) {
        dsc->label_dsc->color = TC(text_head);
        return;
    }
    if ((int)r == sel) {
        dsc->rect_dsc->bg_color     = TC(sel_bg);
        dsc->rect_dsc->border_color = TC(sel_border);
        dsc->rect_dsc->border_width = 2;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM |
                                     (c == 0 ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_RIGHT);
    } else if (is_current(r)) {
        dsc->rect_dsc->bg_color = TC(near_bg);
        if (theme_shape.mono && c == 0) {   /* 1bit: near_bg is the paper; a bar on the left marks it */
            dsc->rect_dsc->border_color = TC(text);
            dsc->rect_dsc->border_width = 4;
            dsc->rect_dsc->border_opa   = LV_OPA_COVER;
            dsc->rect_dsc->border_side  = LV_BORDER_SIDE_LEFT;
        }
    }
    dsc->label_dsc->color = c == 0 ? TC(text_value) : (int)r == sel || is_current(r) ? TC(sel_text) : TC(accent);
}

static void freq_changed(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    if (table) lv_obj_invalidate(table);
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    app_frame_create(&frame, dialog.obj);

    table = lv_table_create(dialog.obj);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    app_frame_body(table);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(table, 10, LV_PART_ITEMS);
    lv_table_set_col_cnt(table, 2);
    lv_table_set_col_width(table, 0, COL0_W);
    lv_table_set_col_width(table, 1, APP_FRAME_W - 2 - COL0_W);
    lv_obj_add_event_cb(table, draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(table, table_click_cb, LV_EVENT_CLICKED, NULL);

    /* The window takes the MFK: the table's own keys would move its own cursor */
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    voice_focus_quiet(dialog.obj);      /* say_sel() speaks the rows */
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    show_head();
    fill();
    lv_obj_update_layout(table);
    select_current();
    {
        char head[120];

        snprintf(head, sizeof(head), "HF Air Band, %s. Small knob: the frequencies, press: tune. F1 area, F3 save, F4 scan",
                 _(aero_area_name(dialog_aero_area())));
        say_sel(head);
    }
    freq_obs = subject_subscribe_delayed((Subject *)cfg_fg_freq, freq_changed, NULL);
}

static void destruct_cb(void) {
    if (freq_obs) param_unsubscribe((Observer *)freq_obs);
    freq_obs = NULL;
    lv_group_remove_obj(dialog.obj);
    table = NULL;
    sel   = -1;
}
