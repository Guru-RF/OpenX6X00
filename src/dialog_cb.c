/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CB: the 27 MHz Citizens Band channels of the plan chosen (cb.c): a row per channel with its frequency, the mode it
 *  is heard in and what the plan keeps it for. The MFK chooses, a press tunes it through the radio's own tuning, in
 *  the plan's mode (FM in Europe and the UK, AM in the Americas and Australia) or the one chosen with Mode, where the
 *  channel allows it: Germany's 41-80 are FM, Australia's 11 AM and 16 LSB, whatever Mode says. With Knob on channels
 *  the main knob steps the channels while the app is open and the radio is inside the plan, 23 between 22 and 24 as
 *  the plan numbers them. Save keeps the channel in the Channels app, Scan hands the plan to the Scanner.
 *
 *  Nothing here keys the radio and nothing here adds a TX rule: PTT on a CB channel follows the radio's own, as
 *  anywhere outside the amateur bands (cfg_tx_refusal): refused ("No TX on 27.185 MHz") unless Settings > TX Cal > TX
 *  outside ham bands is on, and then in the mode the plan allows on that channel, with the radio's power limit and
 *  SWR protection. Transmitting there is the operator's responsibility under their licence (MANUAL.md says so).
 *
 *  Until a plan is chosen it is the country's (Settings > General > STATION > Country: Germany, the UK, Poland and
 *  Australia have their own, the USA, Canada and Mexico the FCC's), else the radio's IARU region's (Europe CEPT, the
 *  Americas FCC, Asia-Pacific Australia's). After condoruy's CX8ANM edition (its CB app); the plans and their modes
 *  are the regulators' now, see cb.c.
 */

#include "dialog_cb.h"

#include "app_frame.h"
#include "band_plan.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "channel_list.h"
#include "countries.h"
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

#define MAX_ROWS 80
#define COL0_W   150
#define COL1_W   200
#define COL2_W   90

static void        construct_cb(lv_obj_t *parent);
static void        destruct_cb(void);
static void        plan_cb(button_data_t *b);
static void        mode_cb(button_data_t *b);
static void        knob_cb(button_data_t *b);
static void        save_cb(button_data_t *b);
static void        scan_cb(button_data_t *b);
static const char *mode_label(void);
static const char *knob_label(void);

static button_data_t btn_plan = {.type = BTN_TEXT, .label = "Plan", .press = plan_cb};
static button_data_t btn_mode = {.type = BTN_TEXT_FN, .label_fn = mode_label, .press = mode_cb};
static button_data_t btn_knob = {.type = BTN_TEXT_FN, .label_fn = knob_label, .press = knob_cb};
static button_data_t btn_save = {.type = BTN_TEXT, .label = "Save\nchannel", .press = save_cb};
static button_data_t btn_scan = {.type = BTN_TEXT, .label = "Scan", .press = scan_cb};

static buttons_page_t page = {.items = {&btn_plan, &btn_mode, &btn_knob, &btn_save, &btn_scan}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
};

dialog_t *dialog_cb = &dialog;

static app_frame_t frame;
static ObserverDelayed *freq_obs;    /* the row the radio is on follows the VFO */
static lv_obj_t   *table;
static int         sel = -1;   /* row: channel - 1 */

int dialog_cb_plan(void) {
    int  p = params.cb_plan.x;
    char cc[3];

    if (p > 0 && p <= CB_PLANS) return p - 1;
    /* None chosen yet: the plan of the country the radio is used in (Settings > General > STATION > Country) when it
     * has one of its own here, else the plan of the band plan's IARU region */
    p = cb_plan_for_country(country_code(cc));
    return p >= 0 ? p : cb_plan_for_region(band_plan_region());
}

cb_mode_t dialog_cb_mode(void) {
    int m = params.cb_mode.x;

    return m > 0 && m <= CB_MODES ? (cb_mode_t)(m - 1) : CB_MODES;
}

int dialog_cb_radio_mode(cb_mode_t mode) {
    switch (mode) {
        case CB_FM:  return radio_mode_nfm;
        case CB_USB: return radio_mode_usb;
        case CB_LSB: return radio_mode_lsb;
        default:     return radio_mode_am;
    }
}

/* The CB mode the radio is in, CB_MODES one of no plan (CW, the digital modes) */
static cb_mode_t radio_cb_mode(void) {
    switch (cparam_i_get(cfg_cur_mode)) {
        case radio_mode_am:
        case radio_mode_sam: return CB_AM;
        case radio_mode_nfm: return CB_FM;
        case radio_mode_usb: return CB_USB;
        case radio_mode_lsb: return CB_LSB;
        default:             return CB_MODES;
    }
}

/* The mode the plan and Mode give */
static cb_mode_t shown_mode(void) {
    cb_mode_t m = dialog_cb_mode();

    return m < CB_MODES && (cb_plan_modes(dialog_cb_plan()) & CB_MODE_BIT(m)) ? m : cb_plan_mode(dialog_cb_plan());
}

static const char *mode_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "%s:\n%s", _("Mode"), cb_mode_name(shown_mode()));
    return buf;
}

static const char *knob_label(void) {
    static char buf[32];

    snprintf(buf, sizeof(buf), "%s:\n%s", _("Knob"), params.cb_knob.x ? _("Channels") : _("Free"));
    return buf;
}

/* The frequency strip's label for a channel's name: the strip puts the app's name, CB, before it */
static const char *strip_label(const char *name) {
    return strncmp(name, "CB ", 3) ? name : name + 3;
}

static int current_channel(void) {
    return cb_channel_at(dialog_cb_plan(), cparam_i_get(cfg_fg_freq));
}

static void show_head(void) {
    lv_label_set_text_fmt(frame.head_left, "CB   #%06x %s#", theme_hex(TC_accent), _(cb_plan_name(dialog_cb_plan())));
    lv_label_set_text_fmt(frame.head_right, "%s", cb_mode_name(shown_mode()));
}

static void show_status(void) {
    int plan = dialog_cb_plan();

    if (sel < 0) {
        lv_label_set_text(frame.status, "MFK: choose - press: tune");
        return;
    }
    char name[24], f[16];
    int  ch = sel + 1;

    lv_label_set_text_fmt(frame.status, "%s - %s MHz - %s", cb_name(plan, ch, name, sizeof(name)),
                          cb_freq_text(cb_freq(plan, ch), f, sizeof(f)),
                          params.cb_knob.x ? _("main knob: channels") : _("main knob: frequency"));
}

static void fill(void) {
    int  plan = dialog_cb_plan(), n = cb_channels(plan);
    char f[16];

    lv_table_set_row_cnt(table, n ? n : 1);
    for (int r = 0; r < n; r++) {
        int ch = r + 1;

        lv_table_set_cell_value_fmt(table, r, 0, "%s %d", _("Channel"), ch);
        lv_table_set_cell_value_fmt(table, r, 1, "%s MHz", cb_freq_text(cb_freq(plan, ch), f, sizeof(f)));
        lv_table_set_cell_value(table, r, 2, cb_mode_name(cb_channel_mode(plan, ch, shown_mode())));
        lv_table_set_cell_value(table, r, 3, cb_channel_note(plan, ch));
    }
}

static void show_sel(void) {
    lv_table_t *t = (lv_table_t *)table;
    lv_coord_t  y = 0;

    if (sel < 0 || !table) return;
    for (int i = 0; i < sel; i++) y += t->row_h[i];
    y += t->row_h[sel] / 2 - lv_obj_get_content_height(table) / 2;
    lv_obj_scroll_to_y(table, y > 0 ? y : 0, LV_ANIM_OFF);
    lv_obj_invalidate(table);
    show_status();
}

static void select_current(void) {
    int ch = current_channel();

    sel = ch > 0 ? ch - 1 : (cb_channels(dialog_cb_plan()) ? 0 : -1);
    show_sel();
}

static void step(int dir) {
    int n = cb_channels(dialog_cb_plan());

    if (sel + dir >= 0 && sel + dir < n) sel += dir;
    show_sel();
}

static void tune_row(int r) {
    int  plan = dialog_cb_plan(), ch = r + 1;
    char name[24], f[16];

    if (ch < 1 || ch > cb_channels(plan)) return;
    if (radio_get_state() != RADIO_RX) {
        msg_update_text_fmt("CB: not while transmitting");
        return;
    }

    int32_t   hz   = cb_freq(plan, ch);
    cb_mode_t want = shown_mode(), mode = cb_channel_mode(plan, ch, want);

    /* The frequency first, as the Channels app: a tune into another band loads that band's mode, replaced then */
    main_screen_set_freq(hz);
    if (cparam_i_get(cfg_cur_mode) != dialog_cb_radio_mode(mode)) cparam_i_set(cfg_cur_mode, dialog_cb_radio_mode(mode));

    cb_name(plan, ch, name, sizeof(name));
    freq_strip_set_label(strip_label(name), hz);
    if (mode != want) {
        msg_update_text_fmt("%s: %s only", name, cb_mode_name(mode));
    } else {
        msg_update_text_fmt("%s %s MHz %s", name, cb_freq_text(hz, f, sizeof(f)), cb_mode_name(mode));
    }
    lv_obj_invalidate(table);
}

bool dialog_cb_knob(int32_t old, int16_t diff) {
    if (!dialog_type_is_run(dialog_cb) || !params.cb_knob.x) return false;     /* every turn of the knob asks */

    int plan = dialog_cb_plan();

    if (!cb_in_plan(plan, old)) return false;
    if (radio_get_state() != RADIO_RX) return true;    /* on the air: no other channel, nor its mode, till it ends */

    int32_t f  = cb_step(plan, old, diff);
    int     ch = cb_channel_at(plan, f);

    if (f == old || ch < 1) return true;   /* the first or the last: it stays there */

    /* As the knob would, then the mode only where the channel does not allow the radio's */
    cparam_i_set(cfg_fg_freq, f);
    if (!(cb_channel_modes(plan, ch) & CB_MODE_BIT(radio_cb_mode())))
        cparam_i_set(cfg_cur_mode, dialog_cb_radio_mode(cb_channel_mode(plan, ch, shown_mode())));

    char name[24];

    channel_overlay_show(cb_name(plan, ch, name, sizeof(name)));
    freq_strip_set_label(strip_label(name), f);
    sel = ch - 1;
    show_sel();
    return true;
}

static void plan_cb(button_data_t *b) {
    (void)b;
    params_uint8_set(&params.cb_plan, (uint8_t)((dialog_cb_plan() + 1) % CB_PLANS + 1));
    params_uint8_set(&params.cb_mode, 0);   /* the new plan's own mode */
    show_head();
    fill();
    select_current();
    buttons_refresh(&btn_mode);
}

static void mode_cb(button_data_t *b) {
    (void)b;
    int      plan    = dialog_cb_plan();
    unsigned allowed = cb_plan_modes(plan);
    int      m       = shown_mode();

    for (int k = 0; k < CB_MODES; k++) {
        m = (m + 1) % CB_MODES;
        if (allowed & CB_MODE_BIT(m)) break;
    }
    if (m == (int)shown_mode()) {
        msg_update_text_fmt("%s: %s only", _(cb_plan_name(plan)), cb_mode_name((cb_mode_t)m));
        return;
    }
    params_uint8_set(&params.cb_mode, (uint8_t)(m + 1));
    show_head();
    fill();
    buttons_refresh(&btn_mode);

    int ch = current_channel();

    if (ch > 0) tune_row(ch - 1);   /* on a channel now: heard in the new mode */
}

static void knob_cb(button_data_t *b) {
    (void)b;
    params_bool_set(&params.cb_knob, !params.cb_knob.x);
    buttons_refresh(&btn_knob);
    show_status();
}

static void save_cb(button_data_t *b) {
    (void)b;
    char name[CHANNEL_NAME_MAX];
    int  ch = current_channel();

    if (ch > 0) cb_name(dialog_cb_plan(), ch, name, sizeof(name));
    else name[0] = 0;
    if (channels_add_named(name)) msg_update_text_fmt("Saved channel %s", name);
    else msg_update_text_fmt("Channel not saved");
}

static void scan_async(void *arg) {
    (void)arg;
    dialog_scan_preset(SCAN_SRC_CB);
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
    if (r >= cb_channels(dialog_cb_plan())) return;
    sel = r;
    show_sel();
    tune_row(sel);
}

static void draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) return;

    int  r = (int)(dsc->id / 4), c = (int)(dsc->id % 4);
    bool cur = r + 1 == current_channel();

    dsc->rect_dsc->bg_opa   = LV_OPA_COVER;
    dsc->rect_dsc->bg_color = TC(panel);
    if (r == sel) {
        dsc->rect_dsc->bg_color     = TC(sel_bg);
        dsc->rect_dsc->border_color = TC(sel_border);
        dsc->rect_dsc->border_width = 2;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM |
                                     (c == 0 ? LV_BORDER_SIDE_LEFT : c == 3 ? LV_BORDER_SIDE_RIGHT : 0);
    } else if (cur) {
        dsc->rect_dsc->bg_color = TC(near_bg);
        if (theme_shape.mono && c == 0) {   /* 1bit: near_bg is the paper; a bar on the left marks it */
            dsc->rect_dsc->border_color = TC(text);
            dsc->rect_dsc->border_width = 4;
            dsc->rect_dsc->border_opa   = LV_OPA_COVER;
            dsc->rect_dsc->border_side  = LV_BORDER_SIDE_LEFT;
        }
    }
    dsc->label_dsc->color = c == 1 ? TC(text_value) : c == 0 ? TC(text) : r == sel || cur ? TC(sel_text) : TC(accent);
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
    lv_table_set_col_cnt(table, 4);
    lv_table_set_col_width(table, 0, COL0_W);
    lv_table_set_col_width(table, 1, COL1_W);
    lv_table_set_col_width(table, 2, COL2_W);
    lv_table_set_col_width(table, 3, APP_FRAME_W - 2 - COL0_W - COL1_W - COL2_W);
    lv_obj_add_event_cb(table, draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(table, table_click_cb, LV_EVENT_CLICKED, NULL);

    /* The window takes the MFK: the table's own keys would move its own cursor */
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    show_head();
    fill();
    lv_obj_update_layout(table);
    select_current();
    freq_obs = subject_subscribe_delayed((Subject *)cfg_fg_freq, freq_changed, NULL);
}

static void destruct_cb(void) {
    if (freq_obs) param_unsubscribe((Observer *)freq_obs);
    freq_obs = NULL;
    lv_group_remove_obj(dialog.obj);
    table = NULL;
    sel   = -1;
}
