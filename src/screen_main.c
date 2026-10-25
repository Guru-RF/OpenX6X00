/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 *
 *  The main screen: spectrum, waterfall and the bars around them, and what the front keys,
 *  the hand mic, the knobs and a USB keyboard do on it. The apps open over it.
 */

#include "screen_main.h"

#include "ft8/ft2.h"
#include "gui_styles.h"
#include "theme.h"
#include "spectrum_view.h"
#include "waterfall_view.h"
#include "util.h"
#include "radio.h"
#include "gui_events.h"
#include "msg_box.h"
#include "msg_note.h"
#include "clock.h"
#include "cw_tune_view.h"
#include "info_bar.h"
#include "smeter.h"
#include "tx_meters.h"
#include "mfk_control.h"
#include "vol_control.h"
#include "main.h"
#include "text_panel.h"
#include "rtty.h"
#include "screen_shot.h"
#include "screen_rec.h"
#include "key_input.h"
#include "app_window.h"
#include "settings_window.h"
#include "freq_entry.h"
#include "cw_messages.h"
#include "dialog_cw_kbd.h"
#include "voice_messages.h"
#include "swrscan_window.h"
#include "ft8_window.h"
#include "gps_window.h"
#include "dialog_field.h"
#include "dialog_bands.h"
#include "dialog_snake.h"
#include "qth_entry.h"
#include "recorder_window.h"
#include "wifi_window.h"
#include "dialog_psk.h"
#include "dialog_olivia.h"
#include "dialog_jtty.h"
#include "app_sstv.h"
#include "dialog_cluster.h"
#include "cluster/client.h"
#include "dialog_modes.h"
#include "dialog_cw_trainer.h"
#include "dialog_wspr.h"
#include "dialog_channels.h"
#include "channels.h"
#include "broadcast_db.h"         /* SWL panel */
#include "navtex_window.h"
#include "wefax_window.h"
#include "dialog_js8.h"
#include "freq_strip.h"
#include "freq_scale.h"
#include "spot_marks.h"
#include "band_plan.h"
#include "cw_field.h"
#include "cluster_column.h"
#include "screen_light.h"
#include "button_bar.h"
#include "recorder.h"
#include "cfg/cfg_api.h"
#include "walkie.h"
#include "cfg/memory.h"
#include "knob_info.h"
#include "radio_controls.h"
#include "apps_menu.h"
#include "dialog_birdie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static lv_obj_t     *obj;                   /* the screen */
static lv_obj_t     *spectrum;              /* also the target of the keys and the MFK press */
static lv_obj_t     *waterfall;
static lv_obj_t     *meter;
static lv_obj_t     *tx_info;

static uint16_t     spectrum_height;        /* Settings > Display > Spectrum height, 76 by default */
static uint16_t     freq_height = 36;       /* the frequency scale under the spectrum, 0 in deck themes */
static lv_coord_t   wf_y;                   /* waterfall top */

/* What an open app keeps the keys from changing. The frequency lock is a subject: the scale
 * greys out with it */
static SubjectInt   *freq_lock;
static bool         band_lock, mode_lock, ab_lock;

static lv_timer_t *timer_once(lv_timer_cb_t cb, uint32_t ms) {
    lv_timer_t *t = lv_timer_create(cb, ms, NULL);

    lv_timer_set_repeat_count(t, 1);
    return t;
}

static bool cw_mode(void) {
    radio_mode_t mode = cparam_i_get(cfg_cur_mode);

    return mode == radio_mode_cw || mode == radio_mode_cwr;
}

static void toggle_freq_lock(void) {
    subject_i_set(freq_lock, !subject_i_get(freq_lock));
}

/* Pop-up with one line of text (the name of the channel FIL/GENE tuned, WEFAX app messages),
 * gone 1 s after the last call */
static lv_obj_t     *note_box;
static lv_obj_t     *note_text;
static lv_timer_t   *note_timer;

static void note_timeout_cb(lv_timer_t *t) {
    if (note_box) {
        lv_obj_del(note_box);
        note_box = note_text = NULL;
    }
    note_timer = NULL;      /* one shot: LVGL deletes it */
}

void channel_overlay_show(const char *text) {
    if (!note_box) {
        note_box = lv_obj_create(obj);
        lv_obj_set_size(note_box, 700, 70);
        lv_obj_align(note_box, LV_ALIGN_TOP_MID, 0, 75);
        lv_obj_set_style_bg_color(note_box, TC(bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(note_box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(note_box, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(note_box, TC(text_mute), LV_PART_MAIN);
        lv_obj_set_style_radius(note_box, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(note_box, 0, LV_PART_MAIN);
        lv_obj_clear_flag(note_box, LV_OBJ_FLAG_SCROLLABLE);

        note_text = lv_label_create(note_box);
        lv_obj_set_width(note_text, 510);
        lv_label_set_long_mode(note_text, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(note_text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(note_text, TC(text), 0);
        lv_obj_set_style_text_font(note_text, &sony_38, 0);
        lv_obj_center(note_text);
    }
    lv_label_set_text(note_text, text && *text ? text : "CHANNEL");
    lv_obj_move_foreground(note_box);

    /* A key pressed again while it shows: the same box, the second starts over */
    if (note_timer) {
        lv_timer_reset(note_timer);
    } else {
        note_timer = timer_once(note_timeout_cb, 1000);
    }
}

/* SWL panel of the hand mic GENE key: the broadcasts on air within 2 kHz of the VFO. GENE again
 * while it shows steps through them, NW saves the one shown as a channel */
#define SWL_TOLERANCE_HZ    2000

static struct {
    lv_obj_t            *panel;     /* NULL when not shown */
    lv_obj_t            *count_lbl;
    lv_obj_t            *name_lbl;
    lv_obj_t            *info_lbl;
    lv_timer_t          *timer;
    broadcast_match_t   found[BROADCAST_MAX_MATCHES];
    size_t              n;
    size_t              cur;
    int32_t             freq;       /* where the lookup was made */
} swl;

static void swl_timeout_cb(lv_timer_t *t) {
    if (swl.panel) {
        lv_obj_del(swl.panel);
        swl.panel = swl.count_lbl = swl.name_lbl = swl.info_lbl = NULL;
    }
    swl.timer = NULL;
    swl.n = swl.cur = 0;
}

static lv_obj_t *swl_label(lv_coord_t x, lv_coord_t y, lv_coord_t w, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(swl.panel);

    if (w) {
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_obj_set_style_text_font(l, font, 0);
    return l;
}

/* The three lines of the panel; the panel is made the first time, styled as the RTTY one */
static void swl_text(const char *count, const char *name, const char *info) {
    if (!swl.panel) {
        swl.panel = lv_label_create(obj);
        lv_label_set_text(swl.panel, "");
        lv_obj_add_style(swl.panel, &panel_style, 0);
        lv_obj_add_flag(swl.panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_clear_flag(swl.panel, LV_OBJ_FLAG_SCROLLABLE);

        swl.count_lbl = swl_label(8, 2, 0, &sony_28);
        swl.name_lbl = swl_label(20, 4, 735, &sony_38);
        lv_label_set_long_mode(swl.name_lbl, LV_LABEL_LONG_DOT);
        swl.info_lbl = swl_label(10, 48, 755, &sony_28);
        lv_label_set_long_mode(swl.info_lbl, LV_LABEL_LONG_WRAP);
    }
    lv_label_set_text(swl.count_lbl, count);
    lv_label_set_text(swl.name_lbl, name);
    lv_label_set_text(swl.info_lbl, info);
}

static void swl_show_current(void) {
    char count[16] = "";
    char info[256];

    if (swl.n == 0) {
        snprintf(info, sizeof(info), "\n%.3f kHz", swl.freq / 1000.0);
        swl_text("", "NO STATION SCHEDULED", info);
    } else {
        const broadcast_match_t *b = &swl.found[swl.cur];

        if (swl.n > 1) {
            snprintf(count, sizeof(count), "%u/%u", (unsigned)swl.cur + 1, (unsigned)swl.n);
        }
        snprintf(info, sizeof(info), "%.3f kHz     %02d:%02d-%02d:%02d UTC\n"
                 "Language: %s     Target: %s\nITU: %s     Days: %s",
                 b->freq_khz, b->start_minute / 60, b->start_minute % 60, b->stop_minute / 60, b->stop_minute % 60,
                 b->language[0] ? b->language : "-", b->target[0] ? b->target : "-",
                 b->itu[0] ? b->itu : "-", b->days[0] ? b->days : "Daily");
        swl_text(count, b->station, info);
    }
    lv_obj_move_foreground(swl.panel);
}

/* Close the panel ms after now, or after the time it already had */
static void swl_close_in(uint32_t ms) {
    if (swl.timer) {
        lv_timer_reset(swl.timer);
    } else {
        swl.timer = timer_once(swl_timeout_cb, ms);
    }
}

static void swl_gene(void) {
    if (!swl.panel) {
        swl.freq = cparam_i_get(cfg_fg_freq);
        swl.n = broadcast_db_find_matches(swl.freq, SWL_TOLERANCE_HZ, swl.found, BROADCAST_MAX_MATCHES);
        swl.cur = 0;
        swl_show_current();
    } else if (swl.n > 1) {
        swl.cur = (swl.cur + 1) % swl.n;    /* the list of the first press, not a new lookup */
        swl_show_current();
    }
    swl_close_in(2000);
}

static void swl_save_failed(const char *why) {
    if (swl.panel) {
        swl_text("", "SAVE ERROR", why);
    } else {
        channel_overlay_show("Channel save error");
    }
}

/* NW: a new channel at the current settings, named after the station shown (or, with no panel,
 * the first one on air here) */
static void swl_save_channel(void) {
    const char          *name = NULL;
    broadcast_match_t   first[BROADCAST_MAX_MATCHES];
    char                station[sizeof(first[0].station)];

    if (swl.panel) {
        if (swl.n > 0) name = swl.found[swl.cur].station;
    } else if (broadcast_db_find_matches(cparam_i_get(cfg_fg_freq), SWL_TOLERANCE_HZ, first,
                                         BROADCAST_MAX_MATCHES) > 0) {
        name = first[0].station;
    }
    if (name) {
        name = strcpy(station, name);
    }

    /* From the file: the list is empty until something loads it */
    const char *why = !channels_load()        ? "Unable to load channels" :
                      !channels_add_current() ? "Unable to add channel" : NULL;

    /* The channel is already written without a name: take it out again */
    if (!why && name && channels_count() > 0 && !channels_set_name(channels_count() - 1, name)) {
        channels_delete(channels_count() - 1);
        why = "Unable to set channel name";
    }
    if (why) {
        swl_save_failed(why);
    } else if (swl.panel) {
        swl_text("", "CHANNEL SAVED", name ? name : "");
        swl_close_in(1000);
        lv_obj_move_foreground(swl.panel);
    } else {
        char text[96];

        snprintf(text, sizeof(text), name && *name ? "Saved channel %s" : "Saved channel", name);
        channel_overlay_show(text);
    }
}

/* Memories 1-9 are the hand mic's (long press on a digit); the backup one keeps the radio
 * as it was while an app moves it, and says nothing */
void mem_load(uint16_t id) {
    bool loaded = cfg_memory_load(id);

    if (!loaded) {
        msg_update_text_fmt("Nothing to load for memory %i", id);
    }
    if (id != MEM_BACKUP_ID) {
        msg_update_text_fmt("Loaded from memory %i", id);   /* even when empty: it covers the line above */
    }
}

void mem_save(uint16_t id) {
    cfg_memory_save(id);
    if (id <= MEM_HKEY_MAX_ID) {
        msg_update_text_fmt("Saved in memory %i", id);
    }
}

/* AGC key and the MFK "AGC" button: Off, Slow, Fast, Auto and round, the order of radio_agc_t */
void main_screen_agc_next() {
    cparam_i_set(cfg_cur_agc, (cparam_i_get(cfg_cur_agc) + 1) % 4);
    if (params.mag_info.x) {
        msg_tiny_set_text_fmt("AGC: %s", info_params_agc());
    }
}

/* The small note in the middle of the screen, when Settings asks for it */
static void tiny_on_off(const char *what, bool on) {
    if (params.mag_info.x) {
        msg_tiny_set_text_fmt("%s: %s", what, on ? "On" : "Off");
    }
}

static void tiny_vfo(void) {
    if (params.mag_info.x) {
        msg_tiny_set_text_fmt("%s", info_params_vfo_label_get());
    }
}

/* The big message for the toggles a key can be given (long press actions) */
static void say_state(const char *what, const char *state) {
    msg_update_text_fmt("#%06x %s: %s", theme_hex(TC_text), what, state);
}

static bool toggle_atu(void) {
    bool on = !param_i_get(cfg_atu_enabled);

    param_i_set(cfg_atu_enabled, on);
    return on;
}

static void step_freq_step(bool up) {
    msg_update_text_fmt("Freq step: %i Hz", cfg_mode_change_freq_step(up));
}

static bool toggle_param(ParamInt *p) {
    bool on = !param_i_get(p);

    param_i_set(p, on);
    return on;
}

static bool toggle_cparam(ComputedParamInt *p) {
    bool on = !cparam_i_get(p);

    cparam_i_set(p, on);
    return on;
}

/* Band up/down, unless an app holds the band; the app hears of it anyway */
static void band_step(bool up) {
    if (!band_lock) {
        cfg_band_load_next(up);
    }
    dialog_send(up ? EVENT_BAND_UP : EVENT_BAND_DOWN, NULL);
}

/* Shown in the frequency strip while the app is open */
static const char *app_name(press_action_t a) {
    switch (a) {
        case ACTION_APP_RTTY:       return "RTTY";
        case ACTION_APP_FT8:        return ftx_protocol_name(param_i_get(cfg_ft8_protocol));
        case ACTION_APP_SWRSCAN:    return "SWR Scan";
        case ACTION_APP_GPS:        return "GPS";
        case ACTION_APP_SETTINGS:   return "Settings";
        case ACTION_APP_RECORDER:   return "Recorder";
        case ACTION_APP_WIFI:       return "WiFi";
        case ACTION_APP_WEFAX:      return "WeFax";
        case ACTION_APP_NAVTEX:     return "NavTex";
        case ACTION_APP_JS8:        return "JS8";
        case ACTION_APP_SSTV:       return "SSTV";
        case ACTION_APP_CLUSTER:    return "Cluster";
        case ACTION_APP_CWTRAINER:  return "CW Trainer";
        case ACTION_APP_WSPR:       return "WSPR";
        case ACTION_APP_CW_KBD:     return "CW Keyboard";
        case ACTION_APP_FIELD:      return "Field Ops";
        case ACTION_APP_BANDS:      return "Bands";
        case ACTION_APP_SNAKE:      return "Snake";
        case ACTION_APP_PSK:        return dialog_psk_is_63() ? "PSK63" : "PSK31";
        case ACTION_APP_OLIVIA:     return "Olivia";
        case ACTION_APP_JTTY:       return "JTTY";
        case ACTION_APP_CHANNELS:   return "Channels";
        case ACTION_APP_BIRDIE:     return "Birdie Cal";
        default:                   return NULL;    /* Modes menu: a menu, not an app */
    }
}

/* Whatever app is open goes: its window, RTTY (the radio back as it was before it), the
 * digital audio path, the panel */
static void close_apps(void) {
    dialog_destruct();
    freq_strip_set_app(NULL);
    if (rtty_get_state() != RTTY_OFF) {
        app_mem_save("RTTY");
        mem_load(MEM_BACKUP_ID);
    }
    rtty_set_state(RTTY_OFF);
    radio_set_digi_cap(false);
    panel_update_visibility(false);
}

/* As the GEN key: the app closes, the GEN buttons come back */
void main_screen_close_app(void) {
    close_apps();
    buttons_load_page_group(buttons_group_gen);
}

void main_screen_start_app(press_action_t app_action) {
    /* Modes again closes its menu. */
    if (app_action == ACTION_APP_MODES && dialog_type_is_run(dialog_modes)) {
        dialog_destruct();
        return;
    }
    close_apps();

    /* The digital apps open where they were last used */
    switch (app_action) {
        case ACTION_APP_FT8: case ACTION_APP_JS8: case ACTION_APP_WSPR: case ACTION_APP_SSTV:
        case ACTION_APP_WEFAX: case ACTION_APP_NAVTEX:
            dialog_app_memory(app_name(app_action));
            radio_set_digi_cap(true);
            break;
        case ACTION_APP_PSK:    /* one memory for PSK31 and PSK63 */
            dialog_app_memory("PSK");
            radio_set_digi_cap(true);
            break;
        case ACTION_APP_OLIVIA:
            dialog_app_memory("Olivia");
            radio_set_digi_cap(true);
            break;
        case ACTION_APP_JTTY:
            dialog_app_memory("JTTY");
            radio_set_digi_cap(true);
            break;
        case ACTION_APP_RTTY:
            radio_set_digi_cap(true);
            break;
        default:
            break;
    }

    /* Every app but RTTY is a window of its own */
    static const struct {
        press_action_t  app;
        dialog_t        **window;
    } windows[] = {
        {ACTION_APP_SETTINGS, &dialog_settings},
        {ACTION_APP_MODES, &dialog_modes},
        {ACTION_APP_SWRSCAN, &dialog_swrscan},
        {ACTION_APP_FT8, &dialog_ft8},
        {ACTION_APP_GPS, &dialog_gps},
        {ACTION_APP_RECORDER, &dialog_recorder},
        {ACTION_APP_WIFI, &dialog_wifi},
        {ACTION_APP_SSTV, &dialog_sstv},
        {ACTION_APP_CLUSTER, &dialog_cluster},
        {ACTION_APP_CWTRAINER, &dialog_cw_trainer},
        {ACTION_APP_WSPR, &dialog_wspr},
        {ACTION_APP_CW_KBD, &dialog_cw_kbd},
        {ACTION_APP_FIELD, &dialog_field},
        {ACTION_APP_BANDS, &dialog_bands},
        {ACTION_APP_SNAKE, &dialog_snake},
        {ACTION_APP_PSK, &dialog_psk},
        {ACTION_APP_OLIVIA, &dialog_olivia},
        {ACTION_APP_JTTY, &dialog_jtty},
        {ACTION_APP_WEFAX, &dialog_wefax},
        {ACTION_APP_NAVTEX, &dialog_navtex},
        {ACTION_APP_JS8, &dialog_js8},
        {ACTION_APP_CHANNELS, &dialog_channels},
        {ACTION_APP_BIRDIE, &dialog_birdie},
    };

    if (app_action == ACTION_APP_RTTY) {
        int32_t freq, mode;

        mem_save(MEM_BACKUP_ID);
        if (app_mem_load("RTTY", &freq, &mode)) {
            main_screen_set_freq(freq);
            cparam_i_set(cfg_cur_mode, mode);
        }
        buttons_load_page(&buttons_page_rtty);
        rtty_set_state(RTTY_RX);
        panel_update_visibility(true);
    }
    for (size_t i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        if (windows[i].app == app_action) {
            dialog_construct(*windows[i].window, obj);
            break;
        }
    }
    freq_strip_set_app(app_name(app_action));
}

/* What a long press (or a hand mic F key) was given in Settings */
void main_screen_action(press_action_t action) {
    switch (action) {
        case ACTION_SCREENSHOT:     screenshot_take(); break;
        case ACTION_RECORDER:       recorder_set_on(!recorder_is_on()); break;
        case ACTION_SCREEN_REC:     screen_rec_toggle(); break;
        case ACTION_MUTE:           radio_change_mute(); break;
        case ACTION_STEP_UP:        step_freq_step(true); break;
        case ACTION_STEP_DOWN:      step_freq_step(false); break;
        case ACTION_ATU_TUNE:       radio_start_atu(); break;   /* as a long press of the ATU key */

        case ACTION_NR_TOGGLE:      say_state("NR", toggle_param(cfg_nr) ? "On" : "Off"); break;
        case ACTION_NB_TOGGLE:      say_state("NB", toggle_param(cfg_nb) ? "On" : "Off"); break;
        case ACTION_ATT_TOGGLE:     say_state("ATT", toggle_cparam(cfg_cur_att) ? "On" : "Off"); break;
        case ACTION_PRE_TOGGLE:     say_state("PRE", toggle_cparam(cfg_cur_pre) ? "On" : "Off"); break;
        case ACTION_ATU_TOGGLE:     say_state("ATU", toggle_atu() ? "On" : "Off"); break;
        case ACTION_WF_MODE:        say_state("Waterfall", toggle_param(cfg_wf_mode) ? "Fixed" : "Centered"); break;

        case ACTION_APP_QTH:        dialog_construct(dialog_qth, obj); break;

        /* Not keys: the callsign is in Settings, Snake has its own hidden press */
        case ACTION_APP_CALLSIGN:
        case ACTION_APP_SNAKE:
            break;

        default:
            /* Voice mode and battery info were removed but may still be stored: nothing */
            if (action >= ACTION_APP_RTTY && action <= ACTION_APP_CHANNELS) {
                main_screen_start_app(action);
            }
            break;
    }
}

/* MSG opens the message dialog; pressed again it turns the page, like the other keys */
static void msg_dialog_open(dialog_t *dialog) {
    if (dialog_type_is_run(dialog)) {
        buttons_page_t *page = buttons_get_cur_page();

        if (page && page->items[0] && page->items[0]->press == button_next_page_cb) {
            buttons_press(0, false);
        }
        return;
    }
    close_apps();
    panel_hide();
    dialog_construct(dialog, obj);
}

/* CW messages in CW, voice messages in the voice modes, nothing in the digital ones */
static void msg_key(void) {
    radio_mode_t mode = cparam_i_get(cfg_cur_mode);

    if (cw_mode()) {
        msg_dialog_open(dialog_msg_cw);
    } else if (mode == radio_mode_lsb || mode == radio_mode_usb || mode == radio_mode_am || mode == radio_mode_nfm ||
               mode == radio_mode_sam || mode == radio_mode_wfm) {
        msg_dialog_open(dialog_msg_voice);
    } else {
        msg_tiny_set_text_fmt("Not used in this mode");
    }
}

/* SSB key: a press swaps LSB and USB, a long press turns DIG on or off; from any other
 * mode it goes to LSB */
static radio_mode_t ssb_key_mode(radio_mode_t mode, bool hold) {
    bool lower = mode == radio_mode_lsb || mode == radio_mode_lsb_dig;
    bool dig   = mode == radio_mode_lsb_dig || mode == radio_mode_usb_dig;

    if (!lower && mode != radio_mode_usb && mode != radio_mode_usb_dig) {
        return radio_mode_lsb;
    }
    if (hold) {
        dig = !dig;
    } else {
        lower = !lower;
    }
    if (lower) {
        return dig ? radio_mode_lsb_dig : radio_mode_lsb;
    }
    return dig ? radio_mode_usb_dig : radio_mode_usb;
}

/* The AM key: AM and NFM, with SAM and WFM between them on a base that has them (the X6200) */
static radio_mode_t am_key_mode(radio_mode_t mode) {
    bool more = (radio_caps() & HW_CAP_SAM_WFM) != 0;

    switch (mode) {
        case radio_mode_am:     return more ? radio_mode_sam : radio_mode_nfm;
        case radio_mode_sam:    return radio_mode_nfm;
        case radio_mode_nfm:    return more ? radio_mode_wfm : radio_mode_am;
        default:                return radio_mode_am;
    }
}

/* The three mode keys, pressed or held: AM/(SAM)/NFM/(WFM), CW/CW-R, SSB as above */
static void mode_key(keypad_key_t key, bool hold) {
    radio_mode_t mode = cparam_i_get(cfg_cur_mode);

    if (key == KEYPAD_MODE_AM) {
        mode = am_key_mode(mode);
    } else if (key == KEYPAD_MODE_CW) {
        mode = mode == radio_mode_cw ? radio_mode_cwr : radio_mode_cw;
    } else {
        mode = ssb_key_mode(mode, hold);
    }
    cparam_i_set(cfg_cur_mode, mode);
}

/* The keys over the buttons: their pages, or the action given to the long press. The hand
 * mic FIL is the DFL key */
static const struct {
    keypad_key_t    key;
    buttons_page_t  **pages;
    uint8_t         *hold_action;
} page_keys[] = {
    {KEYPAD_GEN, buttons_group_gen, &params.long_gen},
    {KEYPAD_APP, buttons_group_app, &params.long_app},
    {KEYPAD_KEY, buttons_group_key, &params.long_key},
    {KEYPAD_DFN, buttons_group_dfn, &params.long_dfn},
    {KEYPAD_DFL, buttons_group_dfl, &params.long_dfl},
};

static bool page_key(keypad_key_t key, bool tap, bool hold) {
    for (size_t i = 0; i < sizeof(page_keys) / sizeof(page_keys[0]); i++) {
        if (page_keys[i].key != key) {
            continue;
        }
        if (tap && key == KEYPAD_APP && params.icon_menus.x) {
            /* The APP grid: APP again closes it, the button pages stay as they are */
            if (dialog_type_is_run(dialog_apps)) {
                dialog_destruct();
            } else {
                close_apps();
                buttons_load_page_group(page_keys[i].pages);
                dialog_construct(dialog_apps, obj);
            }
        } else if (tap) {
            close_apps();
            buttons_load_page_group(page_keys[i].pages);
        } else if (hold) {
            main_screen_action(*page_keys[i].hold_action);
        }
        return true;
    }
    return false;
}

/* PTT: in CW it keys the carrier too */
static void ptt_key(keypad_state_t state) {
    if (state == KEYPAD_PRESS) {
        dialog_msg_voice_ptt_pressed();
        radio_set_ptt(true);
        if (cw_mode()) {
            radio_set_morse_key(true);
        }
    } else if (state == KEYPAD_RELEASE || state == KEYPAD_LONG_RELEASE) {
        if (cw_mode()) {
            radio_set_morse_key(false);
        }
        radio_set_ptt(false);
    }
}

/* Front keys. Most act on the release (a press) or on the long press, not both */
static void front_key_cb(lv_event_t *e) {
    event_keypad_t *ev   = lv_event_get_param(e);
    bool            tap  = ev->state == KEYPAD_RELEASE;
    bool            hold = ev->state == KEYPAD_LONG;

    if (page_key(ev->key, tap, hold)) {
        return;
    }
    switch (ev->key) {
        case KEYPAD_F1: case KEYPAD_F2: case KEYPAD_F3: case KEYPAD_F4: case KEYPAD_F5:
            if (tap || hold) {
                buttons_press(ev->key - KEYPAD_F1, hold);
            }
            break;

        case KEYPAD_MSG:
            if (tap) {
                msg_key();
            } else if (hold) {
                main_screen_action(params.long_msg);
            }
            break;

        case KEYPAD_MODE_AM: case KEYPAD_MODE_CW: case KEYPAD_MODE_SSB:
            if (!mode_lock && (tap || hold)) {
                mode_key(ev->key, hold);
            }
            break;

        case KEYPAD_BAND_UP: case KEYPAD_BAND_DOWN:
            if (tap) {
                band_step(ev->key == KEYPAD_BAND_UP);
            }
            break;

        case KEYPAD_PRE:    /* long: the attenuator */
            if (tap) {
                tiny_on_off("Pre", toggle_cparam(cfg_cur_pre));
            } else if (hold) {
                tiny_on_off("Att", toggle_cparam(cfg_cur_att));
            }
            break;

        case KEYPAD_AGC:    /* long: split */
            if (tap) {
                main_screen_agc_next();
            } else if (hold) {
                toggle_param(cfg_band_split);
                spectrum_clear();
                tiny_vfo();
            }
            break;

        case KEYPAD_FST:    /* long: the step back down */
            if (tap || hold) {
                step_freq_step(tap);
            }
            break;

        case KEYPAD_ATU:    /* long: a tuning cycle */
            if (tap) {
                tiny_on_off("ATU", toggle_atu());
            } else if (hold) {
                radio_start_atu();
            }
            break;

        case KEYPAD_AB:     /* long: this VFO copied to the other one */
            if (ab_lock) {
                break;
            }
            if (tap) {
                radio_toggle_vfo();
                spectrum_clear();
                tiny_vfo();
            } else if (hold) {
                bool on_a = param_i_get(cfg_band_current_vfo) == RADIO_VFO_A;

                cfg_band_vfo_copy();
                msg_update_text_fmt("Clone VFO %s", on_a ? "A->B" : "B->A");
            }
            break;

        case KEYPAD_POWER:  /* a press: the screen off/on */
            if (tap) {
                backlight_switch();
            } else if (hold) {
                msg_update_text_fmt("Power off");
                cluster_client_stop();      /* the cluster link closed, not left to time out at the node */
                radio_poweroff();
            }
            break;

        case KEYPAD_LOCK:   /* long: the base board reset and a restart of the GUI */
            if (tap) {
                toggle_freq_lock();
            } else if (hold) {
                radio_bb_reset();
                exit(1);
            }
            break;

        case KEYPAD_PTT:
            ptt_key(ev->state);
            break;

        case KEYPAD_VM:
            if (tap && !dialog_is_run()) {
                buttons_load_page_group(buttons_group_vm);
            }
            break;

        default:
            LV_LOG_WARN("Unsuported key: %u", ev->key);
            break;
    }
}

/* Hand mic keys */
static void mic_key_cb(lv_event_t *e) {
    event_hkey_t   *ev   = lv_event_get_param(e);
    bool            tap  = ev->state == HKEY_RELEASE;
    bool            hold = ev->state == HKEY_LONG;

    switch (ev->key) {
        /* The band printed on the key; digits only in F-INP (dialog_freq.c). Long: memory 1-9 */
        case HKEY_0: case HKEY_1: case HKEY_2: case HKEY_3: case HKEY_4:
        case HKEY_5: case HKEY_6: case HKEY_7: case HKEY_8: case HKEY_9: {
            static const int16_t band_m[10] = {6, 160, 80, 40, 30, 20, 17, 15, 12, 10};
            int                  digit      = ev->key - HKEY_0;

            if (tap && !band_lock) {
                controls_set_band_m(band_m[digit]);
            } else if (hold && digit > 0) {
                mem_load(digit);
            }
            break;
        }

        case HKEY_SPCH:
            if (tap) {
                toggle_freq_lock();
            }
            break;

        case HKEY_TUNER:    /* as the ATU key, without the note */
            if (tap) {
                toggle_atu();
            } else if (hold) {
                radio_start_atu();
            }
            break;

        case HKEY_XFC:
            if (tap) {
                radio_toggle_vfo();
                spectrum_clear();
            }
            break;

        /* Hand mic UP/DOWN: the channel before/after the last one tuned, its name shown;
         * long press: band up/down */
        case HKEY_UP:
        case HKEY_DOWN: {
            bool up = ev->key == HKEY_UP;

            if (hold) {
                band_step(up);
            } else if (tap && dialog_channels_recall_relative(up ? -1 : +1)) {
                channel_overlay_show(dialog_channels_last_recalled_name());
            }
            break;
        }

        /* Hand mic V/M: the CHANNELS window, not over an app that has the screen */
        case HKEY_VM: {
            bool app_open = dialog_wefax->run || dialog_navtex->run || dialog_sstv->run || dialog_js8->run ||
                            dialog_psk->run || dialog_olivia->run || dialog_jtty->run;

            if (!app_open && tap) dialog_construct(dialog_channels, obj);
            break;
        }

        case HKEY_FIL:
            page_key(KEYPAD_DFL, tap, hold);
            break;

        /* Hand mic MODE: press steps LSB, USB, CW, CW-R, AM, NFM (and SAM, WFM where the base has
         * them); long press is the SSB key's long press (DIG on/off) */
        case HKEY_MODE:
            if (!mode_lock && tap) {
                static const radio_mode_t order[] = {radio_mode_lsb, radio_mode_usb, radio_mode_cw,
                                                     radio_mode_cwr, radio_mode_am,  radio_mode_nfm,
                                                     radio_mode_sam, radio_mode_wfm};
                size_t n_order = radio_caps() & HW_CAP_SAM_WFM ? 8 : 6;
                radio_mode_t mode = cparam_i_get(cfg_cur_mode);
                size_t       i    = 0;

                if (mode == radio_mode_lsb_dig || mode == radio_mode_usb_dig) {
                    mode = mode == radio_mode_lsb_dig ? radio_mode_lsb : radio_mode_usb;
                }
                while (i < n_order && order[i] != mode) {
                    i++;
                }
                cparam_i_set(cfg_cur_mode, order[(i + 1) % n_order]);
            } else if (!mode_lock && hold) {
                mode_key(KEYPAD_MODE_SSB, true);
            }
            break;

        case HKEY_NW:       /* hand mic: save the station shown (or the first one here) as a channel */
        case HKEY_GENE:     /* hand mic: the SWL panel; again: the next station */
            if (tap && ev->key == HKEY_GENE) {
                swl_gene();
            } else if (tap) {
                swl_save_channel();
            }
            break;

        /* F1, F2: the actions given to them in Settings, pressed and held */
        case HKEY_F1:
        case HKEY_F2: {
            bool f1 = ev->key == HKEY_F1;

            if (tap) {
                main_screen_action(f1 ? params.press_f1 : params.press_f2);
            } else if (hold) {
                main_screen_action(f1 ? params.long_f1 : params.long_f2);
            }
            break;
        }

        default:
            LV_LOG_WARN("Unsuported key: %u", ev->key);
            break;
    }
}

/* TX and RX to the widgets that change with them, and to the open app */
static void rx_tx_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    lv_event_send(meter, code, NULL);
    lv_event_send(tx_info, code, NULL);
    lv_event_send(spectrum, code, NULL);
    dialog_send(code, NULL);
}

/* The frequency jumped (a memory, an app): the old spectrum would be a wrong picture */
static void screen_update_cb(lv_event_t *e) {
    spectrum_clear();
}

/* Main knob acceleration from the time per detent (ms, 0: none): grows
 * smoothly with the speed, x2 at dt = k (Lite) or 2k (Strong), then half
 * as steep as upstream's 2^(k / dt), up to x6 (Lite) or x15 (Strong);
 * the onset setting moves k (upstream ae2af22 uses 20 and 40) */
static uint16_t freq_accel(uint16_t dt) {
    float k = 5.0f + 5.0f * param_i_get(cfg_freq_accel_onset);

    if (dt == 0) {
        return 1;
    }
    if (params.freq_accel.x == FREQ_ACCEL_LITE) {
        return LV_MIN(exp2f(0.5f + k / (2.0f * dt)), 6.0f);
    }
    if (params.freq_accel.x == FREQ_ACCEL_STRONG) {
        return LV_MIN(exp2f(0.5f + k / dt), 15.0f);
    }
    return 1;
}

/* Cluster spots on the waterfall: turning slowly (at most spot_snap_slow
 * detents per 33 ms), the VFO stops on a spot it passes and stays there for
 * spot_snap_hold more detents, a light magnetic click (hold 0: off). Turning
 * fast goes straight past. Settings → Display → MAIN SCREEN. Not in CW: the
 * segment is thick with spots and a CW signal is tuned by ear, so the knob
 * kept sticking (ZAP and the app's tap centre a signal instead). */
static int32_t snap_hz;
static int     snap_hold;

/* diff steps of the tuning step, dt ms per detent (0: a key, no acceleration) */
static void plan_mode(int32_t old, int32_t freq);

static void freq_shift(int16_t diff, uint16_t dt) {
    if (subject_i_get(freq_lock)) {
        return;
    }

    int32_t old  = cparam_i_get(cfg_fg_freq);
    int32_t spot;
    int     hold = cw_mode() ? 0 : param_i_get(cfg_spot_snap_hold);
    bool    slow = hold > 0 && (dt == 0 || dt * param_i_get(cfg_spot_snap_slow) >= 33);

    if (slow && snap_hold > 0 && old == snap_hz) {
        snap_hold--;
        return;
    }
    snap_hold = 0;

    int32_t df   = diff * param_i_get(cfg_mode_freq_step) * freq_accel(dt);
    int32_t freq = align_int(old + df, abs(df));

    if (slow && spot_marks_crossed(old, freq, &spot)) {
        freq      = spot;
        snap_hz   = spot;
        snap_hold = hold;
    }
    cparam_i_set(cfg_fg_freq, freq);
    plan_mode(old, freq);
}

/* Settings > General > Mode from band plan: the knob turned into another segment of the band plan
 * sets its mode, CW in a CW segment, the band's sideband in a phone segment. SSB and CW only: the
 * digital modes, AM and FM, and a mode an app holds, stay as they are */
static void plan_mode(int32_t old, int32_t freq) {
    int from = band_plan_type(old), to = band_plan_type(freq);

    if (!param_i_get(cfg_mode_from_plan) || to < 0 || to == from || main_screen_mode_locked()) return;

    int32_t mode = cparam_i_get(cfg_cur_mode);
    bool    ssb  = mode == radio_mode_lsb || mode == radio_mode_usb;
    bool    cw   = mode == radio_mode_cw || mode == radio_mode_cwr;

    if (to == BP_CW && ssb) {
        cparam_i_set(cfg_cur_mode, radio_mode_cw);
    } else if (to == BP_ALL && cw) {
        cparam_i_set(cfg_cur_mode, cfg_default_mode(freq));
    }
}

static void main_knob_cb(lv_event_t *e) {
    rotary_data_t *turn = lv_event_get_param(e);

    freq_shift(turn->diff, turn->dt);
    dialog_rotary(turn->diff);
    free(turn);     /* rotary.c allocates one per event */
}

/* VOL press: turning changes the value, or chooses what VOL controls */
void main_screen_vol_press(void) {
    if (walkie_on()) return;    /* the volume, always */

    bool edit = vol->state != VOL_STATE_EDIT;

    vol->state = edit ? VOL_STATE_EDIT : VOL_STATE_SELECT;
    knobs_set_vol_state(edit);
    vol_update(0);
}

/* MFK as on the main screen: dir -1 / +1 turns it, 0 is the press */
void main_screen_mfk(int dir) {
    if (dir) {
        if (mfk_state == MFK_STATE_EDIT)
            mfk_update(dir);
        else
            mfk_change_ctrl(dir);
        return;
    }

    bool edit = mfk_state != MFK_STATE_EDIT;

    mfk_state = edit ? MFK_STATE_EDIT : MFK_STATE_SELECT;
    knobs_set_mfk_state(edit);
    mfk_update(0);
}

/* Keys sent to the spectrum (it has the keyboard when no app does): the VOL knob, and a USB
 * keyboard with - = to tune, _ + for the step, [ ] { } for VOL, the arrows for the MFK */
static void spectrum_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == KEY_VOL_PRESS) {
        key = LV_KEY_ESC;   /* on the main screen VOL press does what ESC does */
    }
    if (cw_field_key(key)) {
        return;
    }
    switch (key) {
        case '-':                   freq_shift(-1, 0); break;
        case '=':                   freq_shift(+1, 0); break;
        case '_':                   step_freq_step(false); break;
        case '+':                   step_freq_step(true); break;
        case '[':  case KEY_VOL_LEFT_EDIT:      vol_update(-1); break;
        case ']':  case KEY_VOL_RIGHT_EDIT:     vol_update(+1); break;
        case KEY_MFK_PREV:                      mfk_change_ctrl(-1); break;
        case KEY_MFK_NEXT:                      mfk_change_ctrl(+1); break;
        case '{':  case KEY_VOL_LEFT_SELECT:    vol_change_ctrl(-1); break;
        case '}':  case KEY_VOL_RIGHT_SELECT:   vol_change_ctrl(+1); break;
        case LV_KEY_LEFT:           main_screen_mfk(-1); break;
        case LV_KEY_RIGHT:          main_screen_mfk(+1); break;
        case KEYBOARD_PGUP:         band_step(true); break;
        case KEYBOARD_PGDN:         band_step(false); break;
        case KEYBOARD_SCRL_LOCK:    toggle_freq_lock(); break;
        case KEYBOARD_F9:           dialog_construct(dialog_settings, obj); break;

        case KEYBOARD_PRINT:
        case KEYBOARD_PRINT_SCR:
            screenshot_take();
            break;

        case LV_KEY_ESC:
            if (!dialog_is_run()) {
                main_screen_vol_press();
            }
            break;

        case 'f':
        case HKEY_FINP:     /* the frequency typed in */
            if (!subject_i_get(freq_lock)) {
                dialog_construct(dialog_freq, obj);
            }
            break;

        default:
            break;
    }
}

/* MFK press (the spectrum is pressed): edit or choose */
static void spectrum_pressed_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();

    /* The keyboard Enter that sent the CW field line */
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD && cw_field_visible()) return;
    if (dialog_cluster_alert_press()) return;
    main_screen_mfk(0);
}

/* Long MFK press: with the Cluster item active, tune to the highlighted spot.
 * The press already toggled the MFK state; toggle it back to keep browsing. */
static void spectrum_long_pressed_cb(lv_event_t *e) {
    if (mfk_ctrl == CTRL_CLUSTER) {
        cluster_column_tune();
        spectrum_pressed_cb(e);
    }
}

static lv_timer_t *keys_timer;   /* an enable on its way: a dialog opened meanwhile cancels it */

static void keys_enable_cb(lv_timer_t *t) {
    keys_timer = NULL;
    lv_group_add_obj(keyboard_group, spectrum);
    lv_group_set_editing(keyboard_group, true);
}

/* The keys to the spectrum, or not (a dialog takes them) */
void main_screen_keys_enable(bool value) {
    if (value) {
        if (!keys_timer) {
            keys_timer = timer_once(keys_enable_cb, 100);
        }
        return;
    }
    if (keys_timer) {
        lv_timer_del(keys_timer);
        keys_timer = NULL;
    }
    lv_group_remove_obj(spectrum);
    lv_group_set_editing(keyboard_group, false);
}

void main_screen_lock_freq(bool lock) {
    subject_i_set(freq_lock, lock);
}

void main_screen_lock_band(bool lock) {
    band_lock = lock;
}

void main_screen_lock_mode(bool lock) {
    mode_lock = lock;
    info_lock_mode(lock);
}

void main_screen_lock_ab(bool lock) {
    ab_lock = lock;
}

bool main_screen_freq_locked() {
    return subject_i_get(freq_lock);
}

bool main_screen_band_locked() {
    return band_lock;
}

bool main_screen_mode_locked() {
    return mode_lock;
}

/* A jump, not a turn of the knob: the spectrum starts again */
void main_screen_set_freq(uint64_t freq) {
    cparam_i_set(cfg_fg_freq, freq);
    event_send(lv_scr_act(), EVENT_SCREEN_UPDATE, NULL);
}

/* The frequency on the screen: the TX one of a split while transmitting */
static int32_t shown_freq(void) {
    if (param_i_get(cfg_band_split) && radio_get_state() == RADIO_TX) {
        return cparam_i_get(cfg_bg_freq);
    }
    return cparam_i_get(cfg_fg_freq);
}

/* Settings > Display > big frequency: the new frequency in the middle of the screen */
static void on_freq_change(Subject *subj, void *user_data) {
    uint16_t mhz, khz, hz;

    if (!params.mag_freq.x) {
        return;
    }
    split_freq(shown_freq(), &mhz, &khz, &hz);
    if (mhz < 100) {
        msg_tiny_set_text_fmt("%i.%03i.%03i", mhz, khz, hz);
    } else {
        msg_tiny_set_text_fmt("%i.%03i", mhz, khz);    /* VHF and up: no Hz */
    }
}

/* The span of the waterfall changed: the scale, the spots and the band plan follow. The
 * scale is greyed while the frequency is locked */
static void on_span_change(Subject *subj, void *user_data) {
    int32_t  half   = 50000;
    uint32_t color  = theme_hex(subject_i_get(freq_lock) ? TC_text_soft : TC_text);
    int32_t  zoom   = param_i_get(cfg_mode_zoom);

    if (params.waterfall_zoom.x) {
        half /= zoom;
    }

    /* The waterfall is centred on the VFO minus the IF shift */
    /* Fixed waterfall mode: the centre stays put while the VFO moves */
    int32_t center = shown_freq() - param_i_get(cfg_band_if_shift) - waterfall_view_shift();

    freq_scale_update(center, half, color);
    spot_marks_update(center, half);
    band_plan_update(center, half);
}

/* A big IF shift would put the VFO off a zoomed waterfall: zoom out until it is within
 * 40 kHz of the zoomed span */
static void on_if_shift_zoom(Subject *subj, void *user_data) {
    uint32_t shift = abs(param_i_get(cfg_band_if_shift));
    uint32_t zoom  = param_i_get(cfg_mode_zoom);
    uint32_t fit   = zoom;

    while (fit > 1 && shift * fit >= 40000) {
        fit >>= 1;
    }
    if (fit != zoom) {
        param_i_set(cfg_mode_zoom, fit);
    }
}

/* With or without the spectrum, the band plan is a strip at the top of the
 * waterfall and the cluster spots go on the waterfall itself, a short line at
 * its top (across the strip) and the call under it; the waterfall stays where it
 * is (a band of their own for the tags was a tall grey bar that took its room). */
#define SPOT_LINE_H 8

static void on_cluster_spots_wf_change(Subject *subj, void *user_data) {
    lv_coord_t y = wf_y;

    lv_coord_t strip = param_i_get(cfg_band_plan) ? BAND_PLAN_H : 0;

    lv_obj_set_y(waterfall, y);
    freq_scale_lines_set_y(y);
    band_plan_set_y(y);
    spot_marks_layout(y, y + strip + SPOT_LINE_H);
    /* The cluster column starts under the band plan strip, not over it */
    y += strip;
    cluster_column_layout(y);
}

static void on_band_plan_change(Subject *subj, void *user_data) {
    band_plan_refresh();
    on_cluster_spots_wf_change(NULL, NULL);
}

static void on_cluster_column_change(Subject *subj, void *user_data) {
    if (!param_i_get(cfg_cluster_column)) {
        mfk_cluster_gone();
    }
    cluster_column_refresh();
}


/* Under the frequency strip. Spectrum off: it shrinks under the top bars,
 * invisible but still the key target, and the waterfall takes its place.
 * Returns the y under it (the frequency scale) */
static lv_coord_t spectrum_layout(void) {
    lv_coord_t y = 0;

    if (param_i_get(cfg_spectrum_show)) {
        y = FREQ_STRIP_Y + FREQ_STRIP_H + METER_ROW_H;     /* VISION: under the S-meter's row */
        spectrum_height = param_i_get(cfg_spectrum_height);
        lv_obj_remove_local_style_prop(spectrum, LV_STYLE_OPA, 0);
    } else {
        spectrum_height = FREQ_STRIP_Y + FREQ_STRIP_H + METER_ROW_H;
        lv_obj_set_style_opa(spectrum, LV_OPA_TRANSP, 0);
    }
    lv_obj_set_y(spectrum, y);
    lv_obj_set_height(spectrum, spectrum_height);
    return y + spectrum_height;
}

/* Spectrum on/off or its height (Settings, the remote): what is under it moves, live */
static void on_spectrum_layout_change(Subject *subj, void *user_data) {
    lv_coord_t y = spectrum_layout();

    freq_scale_set_y(theme_shape.deck ? y - spectrum_height : y);   /* deck: the edges in the spectrum */
    y += freq_height;
    if (y == wf_y) {
        return;     /* the height of a spectrum that is off */
    }
    wf_y = y;
    waterfall_set_height(480 - y);
    on_cluster_spots_wf_change(NULL, NULL);     /* waterfall, its lines, band plan, spots, cluster column */
    panel_keep_below(wf_y - 8);
}

static void channels_poll_cb(lv_timer_t *t) {
    dialog_channels_poll();
}

static void build(void);

lv_obj_t * main_screen() {
    broadcast_db_init();      /* SWL list for the GENE key, copied to /mnt the first time */
    channels_load();    /* creates channels.json with the defaults when missing */
    lv_timer_create(channels_poll_cb, 2000, NULL);

    freq_lock = subject_i_create(false);
    obj = lv_obj_create(NULL);

    /* The input devices and the radio send their events to the screen */
    lv_obj_add_event_cb(obj, main_knob_cb, EVENT_ROTARY, NULL);
    lv_obj_add_event_cb(obj, front_key_cb, EVENT_KEYPAD, NULL);
    lv_obj_add_event_cb(obj, mic_key_cb, EVENT_HKEY, NULL);
    lv_obj_add_event_cb(obj, rx_tx_cb, EVENT_RADIO_TX, NULL);
    lv_obj_add_event_cb(obj, rx_tx_cb, EVENT_RADIO_RX, NULL);
    lv_obj_add_event_cb(obj, screen_update_cb, EVENT_SCREEN_UPDATE, NULL);

    lv_obj_add_style(obj, &background_style, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    /* On the top layer, over dialogs and the other top-layer items (Dyuldin, upstream da454b48) */
    msg_init(lv_layer_top());
    msg_tiny_init(lv_layer_top());

    build();
    buttons_load_page(&buttons_page_vol_1);
    return obj;
}

/* The widgets: at start, and again on a theme change (main_screen_set_theme()) */
static void build(void) {
    lv_coord_t y;

    spectrum = spectrum_init(obj);
    main_screen_keys_enable(true);

    lv_obj_add_event_cb(spectrum, spectrum_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(spectrum, spectrum_pressed_cb, LV_EVENT_PRESSED, NULL);
    /* Not scrollable: LVGL then sends LONG_PRESSED for the MFK button (it was a no-op before) */
    lv_obj_clear_flag(spectrum, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(spectrum, spectrum_long_pressed_cb, LV_EVENT_LONG_PRESSED, NULL);
    spectrum_min_max_reset();

    freq_height = theme_shape.deck ? 0 : 36;   /* deck: the edge frequencies go in the spectrum's top corners */
    y = spectrum_layout();
    freq_scale_init(obj, theme_shape.deck ? y - spectrum_height : y);

    /* Cluster spots: tags under the frequency labels, lines at the bottom of
     * the spectrum, or at the top of the waterfall when it is off */
    lv_coord_t spot_tag_y = y + 36;

    y += freq_height;
    wf_y = y;
    waterfall = waterfall_init(obj);
    waterfall_min_max_reset();
    lv_obj_set_y(waterfall, y);
    waterfall_set_height(480 - y);
    band_plan_init(obj, y);   /* the top of the waterfall */
    freq_scale_lines_init(obj, y, 480 - y);
    spot_marks_init(obj, y, spot_tag_y);   /* laid out by on_cluster_spots_wf_change() */
    cluster_column_init(obj, wf_y);
    subject_subscribe_delayed_and_notify((Subject*)cfg_cluster_spots_wf, on_cluster_spots_wf_change, NULL);
    subject_subscribe_delayed((Subject*)cfg_band_plan, on_band_plan_change, NULL);
    subject_subscribe_delayed((Subject*)cfg_cluster_column, on_cluster_column_change, NULL);
    subject_subscribe_delayed((Subject*)cfg_spectrum_show, on_spectrum_layout_change, NULL);
    subject_subscribe_delayed((Subject*)cfg_spectrum_height, on_spectrum_layout_change, NULL);

    cw_field_init(obj);
    knobs_init(obj);
    buttons_init(obj);
    panel_init(obj);
    panel_keep_below(wf_y - 8);   /* as with the 76 px spectrum: on the scale's bottom edge */
    info_init(obj);
    clock_init(obj);    /* deck: over the info rows */
    freq_strip_init(obj);
    meter = meter_init(obj);    /* deck: on the frequency panel */
    tx_info = tx_info_init(obj);

    /* Holo: an accent line between the top bar and the frequency strip */
    if (theme_shape.rule) {
        lv_obj_t *rule = lv_obj_create(obj);
        lv_obj_remove_style_all(rule);
        lv_obj_set_pos(rule, 0, TOP_BAR_HEIGHT - TOP_BAR_MARGIN);
        lv_obj_set_size(rule, 800, TOP_BAR_MARGIN);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(rule, TC(accent), 0);
    }

    cw_tune_init(obj);

    /* The order counts: these run in it when several change at once */
    subject_subscribe_delayed((Subject*)freq_lock, on_freq_change, NULL);
    subject_subscribe_delayed((Subject*)cfg_band_split, on_freq_change, NULL);
    subject_subscribe_delayed((Subject*)cfg_fg_freq, on_freq_change, NULL);

    subject_subscribe_delayed((Subject*)freq_lock, on_span_change, NULL);
    subject_subscribe_delayed((Subject*)cfg_fg_freq, on_span_change, NULL);
    subject_subscribe_delayed((Subject*)cfg_band_if_shift, on_span_change, NULL);
    subject_subscribe_delayed((Subject*)cfg_wf_mode, on_span_change, NULL);
    subject_subscribe_delayed_and_notify((Subject*)cfg_mode_zoom, on_span_change, NULL);

    subject_subscribe_delayed_and_notify((Subject*)cfg_bg_freq, on_freq_change, NULL);

    subject_subscribe_delayed((Subject*)cfg_band_if_shift, on_if_shift_zoom, NULL);
    subject_subscribe_delayed((Subject*)cfg_mode_zoom, on_if_shift_zoom, NULL);
}

/* A theme change, live: the open app closes, the main screen is built again with the new
 * theme; the radio and the settings stay as they are. The modules keep their subscriptions,
 * timers and threads (subject_no_duplicates()): they find the new widgets through the same
 * pointers. Not in walkie mode (the buttons are on its screen) */
void main_screen_set_theme(const char *name) {
    close_apps();
    buttons_page_t *page = buttons_get_cur_page();
    buttons_unload_page();
    /* The overlays go with the widgets: closed as their timers would */
    if (note_timer) {
        lv_timer_del(note_timer);
        note_timeout_cb(NULL);
    }
    if (swl.timer) {
        lv_timer_del(swl.timer);
        swl_timeout_cb(NULL);
    }
    lv_async_call_cancel((lv_async_cb_t)lv_obj_invalidate, spectrum);   /* spectrum.c update_dnf() */
    lv_obj_clean(obj);
    walkie_drop();

    styles_load(name);
    subject_no_duplicates(true);
    build();
    subject_no_duplicates(false);
    buttons_load_page(page ? page : &buttons_page_vol_1);
    lv_obj_report_style_change(NULL);   /* what is left: the messages, the top layer */
    event_obj_drop_deleted();           /* what the threads queued for the old widgets */
}

void main_screen_notify_rx_tx(bool tx) {
    event_send(obj, tx ? EVENT_RADIO_TX : EVENT_RADIO_RX, NULL);
}

/* Low battery: a warning, and the radio off 30 s later unless the battery is back by then */
static lv_timer_t *power_off_timer;

static void power_off_cb(lv_timer_t *t) {
    power_off_timer = NULL;     /* one shot: LVGL deletes it */
    msg_update_text_fmt("Power off");
    radio_set_charger(true);
    cluster_client_stop();
    radio_poweroff();
}

void main_screen_notify_low_power(bool is_low) {
    if (!is_low) {
        if (power_off_timer) {
            lv_timer_del(power_off_timer);
            power_off_timer = NULL;
        }
    } else if (!power_off_timer) {
        power_off_timer = timer_once(power_off_cb, 30000);
        msg_schedule_long_text_fmt("Low battery! Turning off in 30s.");
    }
}
