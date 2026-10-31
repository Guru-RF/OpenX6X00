/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Walkie Talkie: a portrait screen (480x800, the knobs on top) in place of
 *  the main one, which is still built and does the radio work unseen. On and
 *  off live: built the first time, then kept.
 */

#include "walkie.h"
#include "board.h"

#include "styles.h"
#include "theme.h"
#include "events.h"
#include "rotary.h"
#include "vol.h"
#include "radio.h"
#include "util.h"
#include "info.h"
#include "meter.h"
#include "spectrum.h"
#include "tx_info.h"
#include "clock.h"
#include "channels.h"
#include "cJSON.h"
#include "buttons.h"
#include "main.h"
#include "main_screen.h"
#include "msg.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "cfg/settings_manager_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W           480
#define SIDE_W      104             /* the bottom buttons, in a column on the left edge */
#define SIDE_KEY_H  160             /* the F keys are 160 px apart, as the landscape buttons */
#define X0          (SIDE_W + 8)    /* the rest of the screen */
#define CW          (W - X0 - 8)
#define XC          ((X0 + CW / 2) - W / 2)     /* centre offset for LV_ALIGN_TOP_MID */
#define STRIP_Y     56
#define STRIP_H     168
#define SCOPE_Y     232             /* the bandscope, its edges and span under it */
#define SCOPE_H     176
#define SCALE_Y     (SCOPE_Y + SCOPE_H + 4)
#define METER_Y     446
#define METER_H     140
#define ROWS_Y      612             /* VOL, SQL, PWR: the name, a bar, the value on one line */
#define ROW_STEP    62
#define BAR_H       12
#define NAME_W      80
#define VAL_W       108

static lv_obj_t *main_scr;              /* the main screen */
static lv_obj_t *main_obj;              /* the same while the walkie screen is up */
static lv_timer_t *timer;
static lv_obj_t *scr, *clock_label, *bat_label, *chan_label, *freq_label, *mode_label;
static lv_obj_t *scope, *scope_lo, *scope_span, *scope_hi;
static lv_obj_t *meter_box, *tx_label, *rxtx, *rxtx_label;
static lv_obj_t *vol_bar, *vol_val, *sql_bar, *sql_val, *pwr_bar, *pwr_val;
static lv_style_t scr_style, strip_style, meter_box_style;     /* boxes of the theme */
static bool     tx;
static float    tx_pwr, tx_swr, tx_alc;
static uint8_t  tx_msg_id;
static cfg_ctrl_t vol_saved;            /* the VOL knob's control before the walkie screen */

static void set_text(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text))
        lv_label_set_text(label, text);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

/* sony_60 is the biggest font: the frequency is zoomed from its centre to fill the strip */
static void freq_set(const char *text) {
    lv_point_t s;

    if (!strcmp(lv_label_get_text(freq_label), text))
        return;
    lv_label_set_text(freq_label, text);
    lv_txt_get_size(&s, text, &sony_60, 0, 0, LV_COORD_MAX, 0);
    lv_obj_set_style_transform_pivot_x(freq_label, s.x / 2, 0);
    lv_obj_set_style_transform_pivot_y(freq_label, s.y / 2, 0);
    lv_obj_set_style_transform_zoom(freq_label, LV_MIN(LV_IMG_ZOOM_NONE * 3 / 2, LV_IMG_ZOOM_NONE * (CW - 24) / s.x), 0);
}

/* A hairline across the content, as under the header */
static void rule(lv_coord_t y) {
    lv_obj_t *o = lv_obj_create(scr);

    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, CW, 1);
    lv_obj_set_pos(o, X0, y);
    lv_obj_set_style_bg_color(o, TC(strip_border), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

/* A level row: the name on the left, a thin bar, the value on the right */
static lv_obj_t *level_row(const char *name, lv_coord_t y, int32_t max, lv_obj_t **value) {
    lv_obj_t *l = label(scr, &sony_26, TC(text_head));
    lv_obj_t *b = lv_bar_create(scr);

    lv_label_set_text(l, name);
    lv_obj_set_pos(l, X0 + 2, y);
    *value = label(scr, &sony_28, TC(text));
    lv_obj_set_width(*value, VAL_W);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(*value, X0 + CW - VAL_W, y - 2);
    lv_obj_set_size(b, CW - NAME_W - VAL_W - 8, BAR_H);
    lv_obj_set_pos(b, X0 + NAME_W, y + (lv_font_get_line_height(&sony_26) - BAR_H) / 2);
    lv_bar_set_range(b, 0, max);
    lv_obj_set_style_radius(b, LV_MIN(theme_shape.radius, BAR_H / 2), LV_PART_MAIN);
    lv_obj_set_style_radius(b, LV_MIN(theme_shape.radius, BAR_H / 2), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(b, LV_MAX(theme_shape.border, 1), LV_PART_MAIN);
    lv_obj_set_style_border_color(b, TC(bar_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, TC(chip), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, TC(text_value), LV_PART_INDICATOR);
    return b;
}

/* One TX bar (0-10) with its name on the left */
static void tx_bar(lv_obj_t *obj, lv_draw_ctx_t *draw_ctx, lv_coord_t y, const char *name, float v, lv_color_t on) {
    lv_draw_rect_dsc_t  dsc;
    lv_draw_label_dsc_t ld;
    lv_area_t           a;

    lv_draw_label_dsc_init(&ld);
    ld.font  = &sony_26;
    ld.color = TC(text_value);
    a.x1 = obj->coords.x1 + 16;
    a.x2 = a.x1 + 70;
    a.y1 = obj->coords.y1 + y - 2;
    a.y2 = a.y1 + 30;
    lv_draw_label(draw_ctx, &ld, &a, name, NULL);

    lv_draw_rect_dsc_init(&dsc);
    a.x1 = obj->coords.x1 + 90;
    a.x2 = obj->coords.x2 - 16;
    a.y1 = obj->coords.y1 + y;
    a.y2 = a.y1 + 24;
    dsc.bg_color = TC(chip);
    if (theme_shape.mono) {     /* 1bit: the track (chip is the paper) framed */
        dsc.border_width = 1;
        dsc.border_color = TC(text);
    }
    lv_draw_rect(draw_ctx, &dsc, &a);
    dsc.border_width = 0;
    a.x2 = a.x1 + (lv_area_get_width(&a) - 1) * LV_CLAMP(0.0f, v, 10.0f) / 10.0f;
    dsc.bg_color = on;
    lv_draw_rect(draw_ctx, &dsc, &a);
}

/* TX: power (0-10 W) and ALC bars instead of the S-meter needle */
static void meter_draw_cb(lv_event_t *e) {
    lv_obj_t      *obj = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);

    if (!tx) {
        meter_needle_draw(obj, draw_ctx);
        return;
    }
    tx_bar(obj, draw_ctx, 18, "PWR", tx_pwr, TC(tx));
    tx_bar(obj, draw_ctx, 54, "ALC", tx_alc, TC(alc));
}

/* The walkie channels: its own list of WCH_N slots, apart from the Channel Memory, in a file
 * of the same format with the slot number ("ch", 1-32) in each entry. freq 0: empty slot */
#define WCH_N       32
#ifndef WCH_FILE
#define WCH_FILE    BOARD_DATA "/walkie_channels.json"
#endif

static channel_t wch[WCH_N];

static void wch_load(void) {
    FILE  *f = fopen(WCH_FILE, "rb");
    char  *text = NULL;
    long   n;
    cJSON *root = NULL, *it;

    memset(wch, 0, sizeof(wch));
    if (!f)
        return;
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) && (text = calloc(1, n + 1)) &&
        fread(text, 1, n, f) == (size_t)n)
        root = cJSON_Parse(text);
    fclose(f);
    free(text);
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "channels")) {
        cJSON    *ch = cJSON_GetObjectItemCaseSensitive(it, "ch");
        cJSON    *name = cJSON_GetObjectItemCaseSensitive(it, "name");
        cJSON    *freq = cJSON_GetObjectItemCaseSensitive(it, "frequency");
        cJSON    *mode = cJSON_GetObjectItemCaseSensitive(it, "mode");
        cJSON    *agc = cJSON_GetObjectItemCaseSensitive(it, "agc");
        channel_t c = {0};

        if (!cJSON_IsNumber(ch) || ch->valueint < 1 || ch->valueint > WCH_N || !cJSON_IsNumber(freq) ||
            freq->valuedouble < 1 || !channels_mode_parse(cJSON_GetStringValue(mode), &c.mode) ||
            !channels_agc_parse(cJSON_GetStringValue(agc), &c.agc))
            continue;
        if (cJSON_IsString(name))
            snprintf(c.name, sizeof(c.name), "%s", name->valuestring);
        c.freq = (uint32_t)freq->valuedouble;
        c.pre = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "pre"));
        c.att = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "att"));
        wch[ch->valueint - 1] = c;
    }
    cJSON_Delete(root);
}

/* Written to a new file, then renamed over the old one: never half a file */
static void wch_save(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *list;
    char  *text;
    FILE  *f;

    cJSON_AddNumberToObject(root, "version", 1);
    list = cJSON_AddArrayToObject(root, "channels");
    for (int i = 0; i < WCH_N; i++) {
        cJSON *it;

        if (!wch[i].freq)
            continue;
        it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "ch", i + 1);
        cJSON_AddStringToObject(it, "name", wch[i].name);
        cJSON_AddNumberToObject(it, "frequency", wch[i].freq);
        cJSON_AddStringToObject(it, "mode", channels_mode_name(wch[i].mode));
        cJSON_AddStringToObject(it, "agc", channels_agc_name(wch[i].agc));
        cJSON_AddBoolToObject(it, "pre", wch[i].pre);
        cJSON_AddBoolToObject(it, "att", wch[i].att);
        cJSON_AddItemToArray(list, it);
    }
    text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text && (f = fopen(WCH_FILE ".new", "wb"))) {
        bool ok = fputs(text, f) >= 0;

        if (fclose(f) == 0 && ok)
            rename(WCH_FILE ".new", WCH_FILE);
    }
    free(text);
}

/* The radio to the slot, as Channel Memory recalls; an empty slot leaves it as it is */
static void wch_recall(int i) {
    const channel_t *c = &wch[i];

    if (!c->freq)
        return;
    cparam_i_set(cfg_fg_freq, c->freq);
    cparam_i_set(cfg_cur_mode, c->mode);
    cparam_i_set(cfg_cur_agc, c->agc);
    cparam_i_set(cfg_cur_pre, c->pre ? radio_pre_on : radio_pre_off);
    cparam_i_set(cfg_cur_att, c->att ? radio_att_on : radio_att_off);
}

/* The main knob in MEM: every slot in turn, the empty ones too (they can be stored into) */
static void mem_step(int dir) {
    int i = (param_i_get(cfg_walkie_ch) + dir + WCH_N) % WCH_N;

    param_i_set(cfg_walkie_ch, i);
    wch_recall(i);
}

static void rotary_cb(lv_event_t *e) {
    rotary_data_t *data = lv_event_get_param(e);

    if (!param_i_get(cfg_walkie_mem)) {
        lv_event_send(main_obj, EVENT_ROTARY, data);    /* the main screen tunes and frees it */
        return;
    }
    for (int i = 0; i < LV_ABS(data->diff) && i < 5; i++)
        mem_step(data->diff > 0 ? 1 : -1);
    free(data);
}

/* The WLK pages: the front keys of the main screen (data->ctrl), short and held, and the
 * walkie's own MEM/VFO and exit */
static void front_key(keypad_key_t key, keypad_state_t state) {
    event_keypad_t ev = { key, state };

    lv_event_send(main_obj, EVENT_KEYPAD, &ev);
}

static void key_press_cb(button_data_t *d) {
    front_key(d->ctrl, KEYPAD_RELEASE);
}

static void key_hold_cb(button_data_t *d) {
    front_key(d->ctrl, KEYPAD_LONG);
}

static void mem_toggle_cb(button_data_t *d) {
    param_i_set(cfg_walkie_mem, !param_i_get(cfg_walkie_mem));
    if (param_i_get(cfg_walkie_mem))
        wch_recall(param_i_get(cfg_walkie_ch));
}

/* Store: the VFO (frequency, mode, AGC, PRE, ATT) into the current slot, then MEM. Clear (held): the slot emptied */
static void store_cb(button_data_t *d) {
    channel_t *c = &wch[param_i_get(cfg_walkie_ch)];

    *c = (channel_t){
        .freq = cparam_i_get(cfg_fg_freq),
        .mode = cparam_i_get(cfg_cur_mode),
        .agc  = cparam_i_get(cfg_cur_agc),
        .pre  = cparam_i_get(cfg_cur_pre) != 0,
        .att  = cparam_i_get(cfg_cur_att) != 0,
    };
    wch_save();
    param_i_set(cfg_walkie_mem, 1);
}

static void clear_cb(button_data_t *d) {
    wch[param_i_get(cfg_walkie_ch)] = (channel_t){0};
    wch_save();
}

static void exit_cb(button_data_t *d) {
    param_i_set(cfg_walkie, 0);         /* on_walkie_change() goes back */
}

#define KEY_BTN(text, key, held) \
    { .type = BTN_TEXT, .label = text, .press = key_press_cb, .hold = held, .ctrl = key }

static buttons_page_t wlk_1, wlk_2, wlk_3, wlk_4;

static button_data_t wlk_p1 = { .type = BTN_TEXT, .label = "(WLK 1:4)", .press = button_next_page_cb,
                                .hold = button_prev_page_cb, .next = &wlk_2, .prev = &wlk_4 };
static button_data_t wlk_p2 = { .type = BTN_TEXT, .label = "(WLK 2:4)", .press = button_next_page_cb,
                                .hold = button_prev_page_cb, .next = &wlk_3, .prev = &wlk_1 };
static button_data_t wlk_p3 = { .type = BTN_TEXT, .label = "(WLK 3:4)", .press = button_next_page_cb,
                                .hold = button_prev_page_cb, .next = &wlk_4, .prev = &wlk_2 };
static button_data_t wlk_p4 = { .type = BTN_TEXT, .label = "(WLK 4:4)", .press = button_next_page_cb,
                                .hold = button_prev_page_cb, .next = &wlk_1, .prev = &wlk_3 };
static button_data_t wlk_mem = { .type = BTN_TEXT, .label = "MEM\nVFO", .press = mem_toggle_cb };
static button_data_t wlk_band_up = KEY_BTN("Band +", KEYPAD_BAND_UP, NULL);
static button_data_t wlk_band_down = KEY_BTN("Band -", KEYPAD_BAND_DOWN, NULL);
static button_data_t wlk_am = KEY_BTN("AM\nFM", KEYPAD_MODE_AM, NULL);
static button_data_t wlk_ssb = KEY_BTN("SSB", KEYPAD_MODE_SSB, key_hold_cb);    /* held: DIG */
static button_data_t wlk_atu = KEY_BTN("ATU", KEYPAD_ATU, key_hold_cb);         /* held: tune, it transmits */
static button_data_t wlk_agc = KEY_BTN("AGC", KEYPAD_AGC, key_hold_cb);         /* held: split */
static button_data_t wlk_pre = KEY_BTN("PRE", KEYPAD_PRE, key_hold_cb);         /* held: ATT */
static button_data_t wlk_ab = KEY_BTN("A/B", KEYPAD_AB, key_hold_cb);           /* held: copy */
static button_data_t wlk_step = KEY_BTN("Step", KEYPAD_FST, key_hold_cb);       /* held: FST held (Settings > Keys) */
static button_data_t wlk_lock = KEY_BTN("Lock", KEYPAD_LOCK, NULL);             /* held would reset */
static button_data_t wlk_exit = { .type = BTN_TEXT, .label = "Exit\nhold", .hold = exit_cb };


static void levels_cb(button_data_t *d) {
    buttons_load_page(&buttons_page_vol_1);
}

static button_data_t wlk_levels = { .type = BTN_TEXT, .label = "Vol Sql\nRF Pwr", .press = levels_cb };
static button_data_t wlk_store = { .type = BTN_TEXT, .label = "Store", .press = store_cb };
static button_data_t wlk_clear = { .type = BTN_TEXT, .label = "Clear\nhold", .hold = clear_cb };

static buttons_page_t wlk_1 = {{&wlk_p1, &wlk_mem, &wlk_band_up, &wlk_band_down, &btn_tx_pwr}};
static buttons_page_t wlk_2 = {{&wlk_p2, &wlk_am, &wlk_ssb, &wlk_atu, &wlk_pre}};
static buttons_page_t wlk_3 = {{&wlk_p3, &wlk_agc, &wlk_ab, &wlk_step, &wlk_lock}};
static buttons_page_t wlk_4 = {{&wlk_p4, &wlk_store, &wlk_clear, &wlk_levels, &wlk_exit}};

static void keypad_cb(lv_event_t *e) {
    event_keypad_t *k = lv_event_get_param(e);

    switch (k->key) {
        case KEYPAD_APP:
            if (k->state == KEYPAD_RELEASE)
                buttons_load_page(&wlk_1);
            break;

        /* The page keys as on the main screen; held they may start an app, unseen here */
        case KEYPAD_GEN:
        case KEYPAD_KEY:
        case KEYPAD_DFN:
        case KEYPAD_DFL:
        case KEYPAD_VM:
            if (k->state == KEYPAD_RELEASE)
                lv_event_send(main_obj, EVENT_KEYPAD, k);
            break;

        case KEYPAD_PTT:            /* the PTT key on the radio's side, with the builtin mic */
        case KEYPAD_F1:
        case KEYPAD_F2:
        case KEYPAD_F3:
        case KEYPAD_F4:
        case KEYPAD_F5:
        case KEYPAD_POWER:
        case KEYPAD_LOCK:
        case KEYPAD_BAND_UP:
        case KEYPAD_BAND_DOWN:
        case KEYPAD_MODE_AM:
        case KEYPAD_MODE_CW:
        case KEYPAD_MODE_SSB:
        case KEYPAD_FST:
        case KEYPAD_PRE:
        case KEYPAD_AB:
        case KEYPAD_ATU:
        case KEYPAD_AGC:
            lv_event_send(main_obj, EVENT_KEYPAD, k);
            break;

        default:
            break;
    }
}

static void update_cb(lv_timer_t *t) {
    char      buf[64];
    time_t    now = time(NULL);
    struct tm tm;
    bool      on_air = radio_get_state() == RADIO_TX;
    uint16_t  mhz, khz, hz;

    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    set_text(clock_label, buf);
    /* No battery fitted, or no reading yet in the first seconds after power-on (radio.c): nothing */
    if (clock_battery_volts() >= 3.0f)
        snprintf(buf, sizeof(buf), "%.1f V", clock_battery_volts());
    else
        buf[0] = '\0';
    set_text(bat_label, buf);

    if (param_i_get(cfg_walkie_mem)) {
        int              i = param_i_get(cfg_walkie_ch);
        const channel_t *c = &wch[i];

        if (!c->freq)
            snprintf(buf, sizeof(buf), "CH %02i - empty", i + 1);
        else if (c->name[0])
            snprintf(buf, sizeof(buf), "CH %02i - %s", i + 1, c->name);
        else
            snprintf(buf, sizeof(buf), "CH %02i", i + 1);
    } else {
        snprintf(buf, sizeof(buf), "VFO");
    }
    set_text(chan_label, buf);

    split_freq(cparam_i_get(cfg_fg_freq), &mhz, &khz, &hz);
    snprintf(buf, sizeof(buf), "%i.%03i.%03i", mhz, khz, hz);
    freq_set(buf);

    /* No repeater offset or tone in this firmware: split is the TX on the other VFO. LOCK: the user's frequency
     * lock (the Lock button) */
    const char *lock = main_screen_freq_user_locked() ? " - LOCK" : "";

    if (param_i_get(cfg_band_split)) {
        split_freq(cparam_i_get(cfg_bg_freq), &mhz, &khz, &hz);
        snprintf(buf, sizeof(buf), "%s - SPLIT %i.%03i.%03i%s", info_params_mode_label_get(), mhz, khz, hz, lock);
    } else {
        snprintf(buf, sizeof(buf), "%s%s", info_params_mode_label_get(), lock);
    }
    set_text(mode_label, buf);

    /* The bandscope: the main screen's spectrum data and drawing (spectrum.c), 10 frames/s */
    float   vfo_at;
    int32_t span = spectrum_span(&vfo_at);
    int32_t lo = cparam_i_get(cfg_fg_freq) - (int32_t)(span * vfo_at);

    snprintf(buf, sizeof(buf), "%.3f", lo / 1e6);
    set_text(scope_lo, buf);
    snprintf(buf, sizeof(buf), "%.3f", (lo + span) / 1e6);
    set_text(scope_hi, buf);
    snprintf(buf, sizeof(buf), "%g kHz", span / 1000.0);
    set_text(scope_span, buf);
    lv_obj_invalidate(scope);

    lv_bar_set_value(vol_bar, param_i_get(cfg_volume), LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%i", (int)param_i_get(cfg_volume));
    set_text(vol_val, buf);
    lv_bar_set_value(sql_bar, param_i_get(cfg_squelch), LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%i", (int)param_i_get(cfg_squelch));
    set_text(sql_val, buf);
    lv_bar_set_value(pwr_bar, (int32_t)(param_f_get(cfg_pwr) * 10.0f + 0.5f), LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%.1f W", param_f_get(cfg_pwr));
    set_text(pwr_val, buf);

    if (on_air != tx) {
        tx = on_air;
        lv_obj_set_style_border_width(scr, tx ? 4 : 0, 0);
        lv_obj_set_style_bg_color(rxtx, tx ? TC(tx_bg) : TC(ok_bg), 0);
        lv_obj_set_style_border_color(rxtx, tx ? TC(tx) : TC(ok), 0);
        lv_obj_set_style_text_color(rxtx_label, tx ? TC(text) : TC(ok), 0);
        lv_label_set_text(rxtx_label, tx ? "TX" : "RX");
        if (!tx)
            lv_label_set_text(tx_label, "");
    }
    if (tx && tx_info_refresh(&tx_msg_id, &tx_alc, &tx_pwr, &tx_swr)) {
        /* SWR from 2 a warning in yellow, from 3 in red (the bar colours of the TX panel) */
        if (tx_swr >= 2.0f)
            snprintf(buf, sizeof(buf), "SWR %.1f HIGH", tx_swr);
        else
            snprintf(buf, sizeof(buf), "%.1f W - SWR %.1f", tx_pwr, tx_swr);
        set_text(tx_label, buf);
        lv_obj_set_style_text_color(tx_label, tx_swr >= 3.0f ? TC(bar_high) : tx_swr >= 2.0f ? TC(bar_mid) : TC(text), 0);
    }
    lv_obj_invalidate(meter_box);       /* the needle moves with meter_update */
}

bool walkie_on(void) {
    return main_obj != NULL;
}

static void build(void) {
    lv_obj_t *o;

    scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    styles_theme_box(&scr_style, TC_bg, TC_bg, TC_bg, 0, TI_bg, W, 800);
    lv_obj_add_style(scr, &scr_style, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);         /* opaque: nothing of the waterfall plane */
    lv_obj_set_style_border_color(scr, TC(tx), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_add_event_cb(scr, rotary_cb, EVENT_ROTARY, NULL);
    lv_obj_add_event_cb(scr, keypad_cb, EVENT_KEYPAD, NULL);

    /* Header: the time and the battery over a rule */
    clock_label = label(scr, &sony_28, TC(text_soft));
    lv_obj_set_pos(clock_label, X0 + 2, 8);
    bat_label = label(scr, &sony_28, TC(text_soft));
    lv_obj_align(bat_label, LV_ALIGN_TOP_RIGHT, -10, 8);
    rule(STRIP_Y - 8);

    /* The channel panel on the theme's strip: VFO or memory and RX/TX, the frequency, the mode */
    o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    styles_theme_box(&strip_style, TC_strip, TC_strip_grad, TC_strip_border, theme_shape.radius,
                     TI_strip, CW, STRIP_H);
    lv_obj_add_style(o, &strip_style, 0);
    lv_obj_set_size(o, CW, STRIP_H);
    lv_obj_set_pos(o, X0, STRIP_Y);
    chan_label = label(o, &sony_28, TC(text_head));
    lv_obj_set_style_max_width(chan_label, CW - 120, 0);     /* a long memory name ends in dots */
    lv_label_set_long_mode(chan_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(chan_label, lv_font_get_line_height(&sony_28));
    lv_obj_set_pos(chan_label, 14, 12);

    rxtx = lv_obj_create(o);
    lv_obj_remove_style_all(rxtx);
    lv_obj_set_size(rxtx, 72, 40);
    lv_obj_set_style_radius(rxtx, LV_MIN(theme_shape.radius, 6), 0);
    lv_obj_set_style_border_width(rxtx, 2, 0);
    lv_obj_set_style_border_color(rxtx, TC(ok), 0);
    lv_obj_set_style_bg_opa(rxtx, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rxtx, TC(ok_bg), 0);
    lv_obj_align(rxtx, LV_ALIGN_TOP_RIGHT, -10, 8);
    rxtx_label = label(rxtx, &sony_28, TC(ok));
    lv_label_set_text(rxtx_label, "RX");
    lv_obj_center(rxtx_label);

    freq_label = label(o, &sony_60, TC(text));
    lv_obj_align(freq_label, LV_ALIGN_TOP_MID, 0, 58);
    mode_label = label(o, &sony_28, TC(text_soft));
    lv_obj_align(mode_label, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* The bandscope on the spectrum's panel colour, framed as the S-meter */
    scope = lv_obj_create(scr);
    lv_obj_remove_style_all(scope);
    lv_obj_set_size(scope, CW, SCOPE_H);
    lv_obj_set_pos(scope, X0, SCOPE_Y);
    lv_obj_set_style_bg_color(scope, TC(panel), 0);
    lv_obj_set_style_bg_opa(scope, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scope, LV_MAX(theme_shape.border, 1), 0);
    lv_obj_set_style_border_color(scope, TC(bar_border), 0);
    lv_obj_set_style_radius(scope, theme_shape.radius, 0);
    lv_obj_set_style_clip_corner(scope, true, 0);
    lv_obj_add_event_cb(scope, spectrum_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    scope_lo = label(scr, &sony_22, TC(text_soft));
    lv_obj_set_pos(scope_lo, X0 + 2, SCALE_Y);
    scope_span = label(scr, &sony_22, TC(text_soft));
    lv_obj_set_width(scope_span, CW);
    lv_obj_set_style_text_align(scope_span, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(scope_span, X0, SCALE_Y);
    scope_hi = label(scr, &sony_22, TC(text_soft));
    lv_obj_set_width(scope_hi, CW / 3);
    lv_obj_set_style_text_align(scope_hi, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(scope_hi, X0 + CW - CW / 3 - 2, SCALE_Y);

    meter_box = lv_obj_create(scr);
    lv_obj_remove_style_all(meter_box);
    styles_theme_box(&meter_box_style, TC_bar, TC_bar_grad, TC_bar_border, theme_shape.radius,
                     TI_bar, CW, METER_H);
    lv_obj_add_style(meter_box, &meter_box_style, 0);
    lv_obj_set_size(meter_box, CW, METER_H);
    lv_obj_set_pos(meter_box, X0, METER_Y);
    lv_obj_set_style_clip_corner(meter_box, true, 0);
    lv_obj_add_event_cb(meter_box, meter_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    tx_label = label(meter_box, &sony_28, TC(text));
    lv_obj_align(tx_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    rule(ROWS_Y - 14);
    vol_bar = level_row("VOL", ROWS_Y, BOARD_VOL_MAX, &vol_val);
    sql_bar = level_row("SQL", ROWS_Y + ROW_STEP, 100, &sql_val);
    pwr_bar = level_row("PWR", ROWS_Y + 2 * ROW_STEP, (int32_t)(BOARD_PWR_MAX * 10), &pwr_val);

    timer = lv_timer_create(update_cb, 100, NULL);
}

/* No digital modes in walkie: whatever brings one (a band or memory with its own mode, CAT,
 * the app, the web remote) goes to plain USB/LSB */
static void no_dig_mode(Subject *subj, void *user_data) {
    if (!walkie_on()) return;
    int32_t mode = cparam_i_get(cfg_cur_mode);
    if (mode != radio_mode_usb_dig && mode != radio_mode_lsb_dig) return;
    cparam_i_set(cfg_cur_mode, mode == radio_mode_usb_dig ? radio_mode_usb : radio_mode_lsb);
    msg_schedule_text_fmt("No digital modes in walkie mode");
}

static void enter(void) {
    if (!main_display_portrait(true)) {
        return;                                     /* fbdev: no portrait */
    }
    main_screen_close_app();                        /* no app or Settings left open under it */
    main_obj = main_scr;
    vol_saved = vol_get_ctrl();
    vol_set_ctrl(CTRL_VOL);                         /* the VOL knob: the volume, given back at leave() */
    radio_set_mic_override(param_i_get(cfg_walkie_mic));   /* Settings > Walkie, built-in by default */
    no_dig_mode(NULL, NULL);
    wch_load();                                     /* each time: the file may have been edited */
    if (!scr) {
        build();
    }
    /* The F1-F5 keys are under the screen in landscape: in portrait they run down the left
     * edge, F1 on top, so the main screen's buttons go there and the knobs work as there */
    buttons_vertical(scr, SIDE_W, SIDE_KEY_H);
    buttons_load_page(&wlk_1);
    lv_timer_resume(timer);
    lv_scr_load(scr);
}

static void leave(void) {
    main_obj = NULL;                                /* walkie_on() false from here */
    lv_timer_pause(timer);
    radio_set_mic_override(-1);                     /* cfg_mic again, never changed */
    vol_set_ctrl(vol_saved);
    buttons_horizontal(main_scr);
    buttons_load_page(&buttons_page_vol_1);         /* as at start */
    main_display_portrait(false);
    lv_scr_load(main_scr);
}

static void on_walkie_change(Subject *subj, void *user_data) {
    bool on = param_i_get(cfg_walkie);

    if (on && !walkie_on()) {
        enter();
    } else if (!on && walkie_on()) {
        leave();
    }
}

/* A theme change (main_screen_set_theme()): made again with the new theme at the next enter() */
void walkie_drop(void) {
    if (scr) {
        lv_timer_del(timer);
        lv_obj_del(scr);
        scr = NULL;
    }
}

void walkie_init(lv_obj_t *main_screen_obj) {
    main_scr = main_screen_obj;
    subject_subscribe_delayed_and_notify((Subject *)cfg_walkie, on_walkie_change, NULL);
    subject_subscribe_delayed((Subject *)cfg_cur_mode, no_dig_mode, NULL);
}
