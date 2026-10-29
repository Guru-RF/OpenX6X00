/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  The five buttons over F1-F5: the pages of the front keys, the slots on screen that show
 *  whatever page is loaded (an app's too), and the knob marks on the buttons.
 */
#include "button_bar.h"

#include "radio_controls.h"
#include "util.h"
#include "radio.h"
#include "tx_player.h"
#include "cfg/settings_manager.h"
#include "cfg/encoder_defaults.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "screen_rec.h"
#include "msg_box.h"
#include "walkie.h"
#include "cw_field.h"
}

#define STATE_ASSIGNED LV_STATE_USER_1  /* bound to a knob in the knob binds */
#define STATE_ACTIVE   LV_STATE_USER_2  /* the knob is on this control now */

/* One of the five places on screen */
struct _disp_button_t {
    lv_obj_t      *parent;    /* the button itself */
    lv_obj_t      *label;
    lv_obj_t      *value;     /* second line of a "Name:\nvalue" label */
    lv_obj_t      *vol_mark;
    lv_obj_t      *mfk_mark;
    button_data_t *data;      /* what it shows, or NULL */
};

static disp_btn_t      slots[BUTTONS];
static buttons_page_t *cur_page;
static std::string     knob_binds;   /* cfg encoder_bind as last notified: a char a control */

static void slot_refresh(disp_btn_t *s);
static void set_btn_text(disp_btn_t *b, const char *text);

/* ---- what the buttons do ---- */

/* The controls the VOL knob takes at a press; it has them from the start */
static bool vol_by_default(cfg_ctrl_t ctrl) {
    return std::find(kEncoderVolDefaults.begin(), kEncoderVolDefaults.end(), ctrl) != kEncoderVolDefaults.end();
}

/* Press: the control to its knob. The knob is the default one, not the bound one; on
 * the walkie screen the VOL knob stays the volume */
static void knob_press(button_data_t *d) {
    cfg_ctrl_t ctrl = (cfg_ctrl_t)d->ctrl;

    if (ctrl < CTRL_FAST_ACCESS_LAST && vol_by_default(ctrl) && !walkie_on()) {
        vol_set_ctrl(ctrl);
    } else {
        mfk_set_ctrl(ctrl);
    }
}

/* Hold: the next bind of the control, none -> VOL -> MFK -> none */
static void knob_bind_hold(button_data_t *d) {
    std::string binds = cfg_sm.p_encoder_bind.get();

    if ((size_t)d->ctrl >= binds.size()) return;
    char &b = binds[d->ctrl];
    switch (b) {
        case ENCODER_BIND_NONE:
            msg_update_text_fmt("Added to VOL encoder");
            b = ENCODER_BIND_VOL;
            break;
        case ENCODER_BIND_VOL:
            msg_update_text_fmt("Moved to MFK encoder");
            b = ENCODER_BIND_MFK;
            break;
        case ENCODER_BIND_MFK:
            msg_update_text_fmt("Removed from MFK encoder");
            b = ENCODER_BIND_NONE;
            break;
        default:
            b = ENCODER_BIND_NONE;
    }
    cfg_sm.p_encoder_bind.set(binds);
}

static void app_press(button_data_t *d) {
    main_screen_start_app((press_action_t)d->ctrl);
}

static void mem_press(button_data_t *d) {
    mem_load(d->ctrl);
}

static void mem_hold(button_data_t *d) {
    mem_save(d->ctrl);
}

/* ---- labels ---- */

/* One buffer for all: a label is copied by the button before the next is made */
static const char *text(const char *fmt, ...) {
    static char buf[32];
    va_list     ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static const char *on_off(int32_t v) {
    return v ? "On" : "Off";
}

static const char *agc_label() {
    static const char *const names[] = {"Off", "Slow", "Fast", "Auto"};
    int32_t                  agc = cfg_sm.cp_cur_agc.get();

    return text("AGC:\n%s", agc >= 0 && agc <= radio_agc_auto ? names[agc] : "?");
}

static const char *screen_rec_label() {
    return screen_rec_is_on() ? "Stop\nrec" : "Screen\nrec";
}

static const char *wf_mode_label() {
    return cfg_sm.p_wf_mode.get() ? "WF\nFixed" : "WF\nCenter";
}

/* Label shows the span the button selects */
static const char *span_label(int32_t zoom) {
    static char buf[2][24];
    static int  n;

    n ^= 1;
    snprintf(buf[n], sizeof buf[n], "Span %s\n%g kHz", zoom < cfg_sm.p_mode_zoom.get() ? "+" : "-", 100.0 / zoom);
    return buf[n];
}

static const char *span_wider_label() {
    int32_t z = cfg_sm.p_mode_zoom.get();
    return z > 1 ? span_label(z >> 1) : "Span +\nmax";
}

static const char *span_narrower_label() {
    int32_t z = cfg_sm.p_mode_zoom.get();
    return z < 8 ? span_label(z << 1) : "Span -\nmin";
}

/* TX power in the digital apps: press for the next step, up to the apps' 7 W cap */
static const char *dig_pwr_label() {
    static char buf[20];
    snprintf(buf, sizeof(buf), "Power\n%g W", LV_MIN(cfg_sm.p_pwr.get(), TX_PLAYER_MAX_PWR_W));
    return buf;
}

/* The setting itself (the radio follows it): radio_set_pwr() alone left the label and the
 * next step on the old value, and the transmission levelled to it */
static void dig_pwr_cb(button_data_t *data) {
    static const float steps[] = {0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 3.0f, 5.0f, TX_PLAYER_MAX_PWR_W};
    float p = LV_MIN(cfg_sm.p_pwr.get(), TX_PLAYER_MAX_PWR_W), next = steps[0];

    for (float s : steps) {
        if (s > p + 0.05f) {
            next = s;
            break;
        }
    }
    cfg_sm.p_pwr.set(next);
    /* A step up that the setting did not take is the top it allows now (the X6200 on battery: 5 W),
     * so round to the first: the button stayed on 5 W there */
    if (next > p && cfg_sm.p_pwr.get() < p + 0.05f) {
        cfg_sm.p_pwr.set(steps[0]);
    }
}

/* ---- the buttons ---- */

using label_fn_t = const char *(*)();
using action_t   = void (*)(button_data_t *);

/* A value on a knob: press takes the knob, hold changes the bind */
static button_data_t knob(label_fn_t label, cfg_ctrl_t ctrl, void *subj) {
    return {.type = BTN_TEXT_FN, .label_fn = label, .press = knob_press, .hold = knob_bind_hold,
            .ctrl = ctrl, .encoder_allowed = true, .subj = (Subject *)subj};
}

/* An on/off switched by the press, on a knob too */
static button_data_t knob_toggle(label_fn_t label, action_t toggle, cfg_ctrl_t ctrl, void *subj) {
    button_data_t b = knob(label, ctrl, subj);
    b.press = toggle;
    return b;
}

static button_data_t page_btn(const char *name) {
    return {.type = BTN_TEXT, .label = name, .press = button_next_page_cb, .hold = button_prev_page_cb};
}

static button_data_t app_btn(const char *name, press_action_t app) {
    return {.type = BTN_TEXT, .label = name, .press = app_press, .ctrl = app};
}

static button_data_t mem_btn(const char *name, int32_t mem) {
    return {.type = BTN_TEXT, .label = name, .press = mem_press, .hold = mem_hold, .ctrl = mem};
}

static button_data_t fn_btn(label_fn_t label, action_t press, void *subj) {
    return {.type = BTN_TEXT_FN, .label_fn = label, .press = press, .subj = (Subject *)subj};
}

/* VOL */
static button_data_t btn_vol = {.type = BTN_TEXT_FN,
                                .label_fn = [] { return text("Volume:\n%i", cfg_sm.p_volume.get()); },
                                .press = knob_press, .ctrl = CTRL_VOL, .encoder_allowed = true,
                                .subj = (Subject *)&cfg_sm.p_volume};   /* never off its knob */
static button_data_t btn_sql = knob([] { return text("Squelch:\n%i", cfg_sm.p_squelch.get()); },
                                    CTRL_SQL, &cfg_sm.p_squelch);
static button_data_t btn_rfg = knob([] { return text("RF gain:\n%i", cfg_sm.p_rfgain.get()); },
                                    CTRL_RFG, &cfg_sm.p_rfgain);
button_data_t btn_tx_pwr = knob([] { return text("TX power:\n%0.1f W", cfg_sm.p_pwr.get()); },
                                CTRL_PWR, &cfg_sm.p_pwr);
button_data_t btn_dig_pwr = fn_btn(dig_pwr_label, dig_pwr_cb, &cfg_sm.p_pwr);
static button_data_t btn_moni = knob([] { return text("Moni level:\n%i", cfg_sm.p_moni.get()); },
                                     CTRL_MONI, &cfg_sm.p_moni);

/* DFL */
static button_data_t btn_flt_low = knob([] { return text("Filter low:\n%i Hz", cfg_sm.cp_cur_filter_low.get()); },
                                        CTRL_FILTER_LOW, &cfg_sm.cp_cur_filter_low);
static button_data_t btn_flt_high = knob([] { return text("Filter high:\n%i Hz", cfg_sm.cp_cur_filter_high.get()); },
                                         CTRL_FILTER_HIGH, &cfg_sm.cp_cur_filter_high);
static button_data_t btn_flt_bw = knob([] { return text("Filter BW:\n%i Hz", cfg_sm.cp_cur_filter_bw.get()); },
                                       CTRL_FILTER_BW, &cfg_sm.cp_cur_filter_bw);

/* MFK */
static button_data_t btn_rit = knob([] { return text("RIT:\n%+i", cfg_sm.p_rit.get()); }, CTRL_RIT, &cfg_sm.p_rit);
static button_data_t btn_xit = knob([] { return text("XIT:\n%+i", cfg_sm.p_xit.get()); }, CTRL_XIT, &cfg_sm.p_xit);
static button_data_t btn_zoom = {.type = BTN_TEXT, .label = "Spectrum\nZoom", .press = knob_press,
                                 .hold = knob_bind_hold, .ctrl = CTRL_SPECTRUM_FACTOR, .encoder_allowed = true};
static button_data_t btn_wf_ref = knob([] { return text("WF ref:\n%+i dB", cfg_sm.p_waterfall_ref.get()); },
                                       CTRL_WF_REF, &cfg_sm.p_waterfall_ref);
static button_data_t btn_agc = fn_btn(agc_label, [](button_data_t *) { main_screen_agc_next(); }, &cfg_sm.cp_cur_agc);
/* PRE and ATT: the X6200 has no PRE key (the X6100's PRE key, held: ATT), and its sensitivity is specified with
 * the preamp on; per band, as the keys set them */
static button_data_t btn_pre = fn_btn([] { return text("PRE:\n%s", on_off(cfg_sm.cp_cur_pre.get())); },
                                      [](button_data_t *b) {
                                          cfg_sm.cp_cur_pre.set(!cfg_sm.cp_cur_pre.get());
                                          buttons_refresh(b);
                                      }, &cfg_sm.cp_cur_pre);
static button_data_t btn_att = fn_btn([] { return text("ATT:\n%s", on_off(cfg_sm.cp_cur_att.get())); },
                                      [](button_data_t *b) {
                                          cfg_sm.cp_cur_att.set(!cfg_sm.cp_cur_att.get());
                                          buttons_refresh(b);
                                      }, &cfg_sm.cp_cur_att);
static button_data_t btn_comp = knob([] { return text("Comp:\n%s", params_comp_str_get(cfg_sm.p_comp.get())); },
                                     CTRL_COMP, &cfg_sm.p_comp);
static button_data_t btn_if_shift = knob([] { return text("IF shift:\n%i", cfg_sm.p_band_if_shift.get()); },
                                         CTRL_IF_SHIFT, &cfg_sm.p_band_if_shift);
static button_data_t btn_vox = knob_toggle([] { return text("VOX:\n%s", on_off(cfg_sm.p_vox_en.get())); },
                                           controls_toggle_vox, CTRL_VOX_ON, &cfg_sm.p_vox_en);
static button_data_t btn_wf_mode = knob_toggle(wf_mode_label, controls_toggle_wf_mode, CTRL_WF_MODE, &cfg_sm.p_wf_mode);
static button_data_t btn_span_wider = fn_btn(span_wider_label, controls_span_wider, &cfg_sm.p_mode_zoom);
static button_data_t btn_span_narrower = fn_btn(span_narrower_label, controls_span_narrower, &cfg_sm.p_mode_zoom);
static button_data_t btn_screen_rec = fn_btn(screen_rec_label, [](button_data_t *) { screen_rec_toggle(); }, NULL);

/* KEY */
static button_data_t btn_key_speed = knob([] { return text("Speed:\n%i wpm", cfg_sm.p_key_speed.get()); },
                                          CTRL_KEY_SPEED, &cfg_sm.p_key_speed);
static button_data_t btn_key_vol = knob([] { return text("Volume:\n%i", cfg_sm.p_key_vol.get()); },
                                        CTRL_KEY_VOL, &cfg_sm.p_key_vol);
static button_data_t btn_key_train = knob_toggle([] { return text("Train:\n%s", on_off(cfg_sm.p_key_train.get())); },
                                                 controls_toggle_key_train, CTRL_KEY_TRAIN, &cfg_sm.p_key_train);
static button_data_t btn_key_tone = knob([] { return text("Tone:\n%i Hz", cfg_sm.p_key_tone.get()); },
                                         CTRL_KEY_TONE, &cfg_sm.p_key_tone);
static button_data_t btn_key_mode = knob(
    [] { return text("Mode:\n%s", params_key_mode_str_get((radio_key_mode_t)cfg_sm.p_key_mode.get())); },
    CTRL_KEY_MODE, &cfg_sm.p_key_mode);
static button_data_t btn_iambic = knob_toggle(
    [] { return text("Iambic:\n%s mode", params_iambic_mode_str_ger((radio_iambic_mode_t)cfg_sm.p_iambic_mode.get())); },
    controls_toggle_key_iambic_mode, CTRL_IAMBIC_MODE, &cfg_sm.p_iambic_mode);
static button_data_t btn_qsk = knob([] { return text("QSK time:\n%i ms", cfg_sm.p_qsk_time.get()); },
                                    CTRL_QSK_TIME, &cfg_sm.p_qsk_time);
static button_data_t btn_ratio = knob([] { return text("Ratio:\n%0.1f", cfg_sm.p_key_ratio.get()); },
                                      CTRL_KEY_RATIO, &cfg_sm.p_key_ratio);

/* CW decoder */
static button_data_t btn_cw_zap = {.type = BTN_TEXT, .label = "CW\nZAP", .press = controls_cw_zap};
static button_data_t btn_cw_peak = knob_toggle([] { return text("CW peak:\n%s", on_off(cfg_sm.p_cw_peak_on.get())); },
                                               controls_toggle_cw_peak, CTRL_CW_PEAK_ON, &cfg_sm.p_cw_peak_on);
static button_data_t btn_cw_peak_q = knob([] { return text("CW peak Q:\n%i", cfg_sm.p_cw_peak_q.get()); },
                                          CTRL_CW_PEAK_Q, &cfg_sm.p_cw_peak_q);
static button_data_t btn_cw_log = {.type = BTN_TEXT, .label = "Log\nQSO",
                                   .press = [](button_data_t *) { cw_field_log_open(); }};
static button_data_t btn_cw_decoder = knob_toggle(
    [] { return text("Decoder:\n%s", on_off(cfg_sm.p_cw_decoder.get())); },
    controls_toggle_cw_decoder, CTRL_CW_DECODER, &cfg_sm.p_cw_decoder);
static button_data_t btn_cw_tuner = knob_toggle([] { return text("Tuner:\n%s", on_off(cfg_sm.p_cw_tune.get())); },
                                                controls_toggle_cw_tuner, CTRL_CW_TUNE, &cfg_sm.p_cw_tune);
static button_data_t btn_cw_snr = knob([] { return text("Dec SNR:\n%0.1f dB", cfg_sm.p_cw_decoder_snr.get()); },
                                       CTRL_CW_DECODER_SNR, &cfg_sm.p_cw_decoder_snr);
static button_data_t btn_cw_parse = fn_btn([] { return cfg_sm.p_cw_parse.get() ? "Parse:\nOn" : "Parse:\nOff"; },
                                           [](button_data_t *) { cfg_sm.p_cw_parse.set(!cfg_sm.p_cw_parse.get()); },
                                           &cfg_sm.p_cw_parse);

/* DFN */
static button_data_t btn_dnf = knob_toggle([] { return text("DNF:\n%s", on_off(cfg_sm.p_dnf.get())); },
                                           controls_toggle_dnf, CTRL_DNF, &cfg_sm.p_dnf);
static button_data_t btn_dnf_center = knob([] { return text("DNF freq:\n%i Hz", cfg_sm.p_dnf_center.get()); },
                                           CTRL_DNF_CENTER, &cfg_sm.p_dnf_center);
static button_data_t btn_dnf_width = knob([] { return text("DNF width:\n%i Hz", cfg_sm.p_dnf_width.get()); },
                                          CTRL_DNF_WIDTH, &cfg_sm.p_dnf_width);
static button_data_t btn_dnf_auto = knob_toggle([] { return text("DNF auto:\n%s", on_off(cfg_sm.p_dnf_auto.get())); },
                                                controls_toggle_dnf_auto, CTRL_DNF_AUTO, &cfg_sm.p_dnf_auto);
static button_data_t btn_nb = knob_toggle([] { return text("NB:\n%s", on_off(cfg_sm.p_nb.get())); },
                                          controls_toggle_nb, CTRL_NB, &cfg_sm.p_nb);
static button_data_t btn_nb_level = knob([] { return text("NB level:\n%i", cfg_sm.p_nb_level.get()); },
                                         CTRL_NB_LEVEL, &cfg_sm.p_nb_level);
static button_data_t btn_nb_width = knob([] { return text("NB width:\n%i Hz", cfg_sm.p_nb_width.get()); },
                                         CTRL_NB_WIDTH, &cfg_sm.p_nb_width);
static button_data_t btn_nr = knob_toggle([] { return text("NR:\n%s", on_off(cfg_sm.p_nr.get())); },
                                          controls_toggle_nr, CTRL_NR, &cfg_sm.p_nr);
static button_data_t btn_nr_level = knob([] { return text("NR level:\n%i", cfg_sm.p_nr_level.get()); },
                                         CTRL_NR_LEVEL, &cfg_sm.p_nr_level);

/* VM: held saves the memory, a press loads it */
static button_data_t btn_mem[] = {mem_btn("Set 1", 1), mem_btn("Set 2", 2), mem_btn("Set 3", 3), mem_btn("Set 4", 4),
                                  mem_btn("Set 5", 5), mem_btn("Set 6", 6), mem_btn("Set 7", 7), mem_btn("Set 8", 8)};

/* APP; the digital mode apps are in the Modes menu */
static button_data_t btn_modes = app_btn("Modes", ACTION_APP_MODES);
static button_data_t btn_cluster = app_btn("Cluster", ACTION_APP_CLUSTER);
static button_data_t btn_swr = app_btn("SWR\nScan", ACTION_APP_SWRSCAN);
static button_data_t btn_rec = app_btn("RX\nRecorder", ACTION_APP_RECORDER);
static button_data_t btn_settings = app_btn("Settings", ACTION_APP_SETTINGS);
static button_data_t btn_cwt = app_btn("CW\nTrainer", ACTION_APP_CWTRAINER);
static button_data_t btn_cwkbd = app_btn("CW\nKeyboard", ACTION_APP_CW_KBD);
static button_data_t btn_field = app_btn("Field\nOps", ACTION_APP_FIELD);
static button_data_t btn_chans = app_btn("Channels", ACTION_APP_CHANNELS);
static button_data_t btn_bands = app_btn("Bands", ACTION_APP_BANDS);
static button_data_t btn_gps = {.type = BTN_TEXT, .label = "GPS", .press = app_press,
                                .hold = [](button_data_t *) { main_screen_start_app(ACTION_APP_SNAKE); },  /* the hidden game */
                                .ctrl = ACTION_APP_GPS};

/* RTTY: two pages of their own */
static button_data_t btn_rtty_rate = {.type = BTN_TEXT, .label = "Rate", .press = knob_press,
                                      .ctrl = CTRL_RTTY_RATE, .encoder_allowed = true};
static button_data_t btn_rtty_shift = {.type = BTN_TEXT, .label = "Shift", .press = knob_press,
                                       .ctrl = CTRL_RTTY_SHIFT, .encoder_allowed = true};
static button_data_t btn_rtty_center = {.type = BTN_TEXT, .label = "Center", .press = knob_press,
                                        .ctrl = CTRL_RTTY_CENTER, .encoder_allowed = true};
static button_data_t btn_rtty_reverse = {.type = BTN_TEXT, .label = "Reverse", .press = knob_press,
                                         .ctrl = CTRL_RTTY_REVERSE, .encoder_allowed = true};
static button_data_t btn_rtty_p2 = {.type = BTN_TEXT, .label = "(RTTY 2:2)", .press = button_next_page_cb,
                                    .hold = button_prev_page_cb, .next = &buttons_page_rtty, .prev = &buttons_page_rtty};
static buttons_page_t page_rtty_2 = {{&btn_rtty_p2, &btn_dig_pwr}};
static button_data_t btn_rtty_p1 = {.type = BTN_TEXT, .label = "(RTTY 1:2)", .press = button_next_page_cb,
                                    .hold = button_prev_page_cb, .next = &page_rtty_2, .prev = &page_rtty_2};
buttons_page_t buttons_page_rtty = {{&btn_rtty_p1, &btn_rtty_rate, &btn_rtty_shift, &btn_rtty_center, &btn_rtty_reverse}};

/* ---- pages and groups; buttons_init() links the page buttons of a group in a ring ---- */

static button_data_t btn_vol_p1 = page_btn("(VOL 1:2)"), btn_vol_p2 = page_btn("(VOL 2:2)");
static button_data_t btn_mfk_p1 = page_btn("(MFK 1:3)"), btn_mfk_p2 = page_btn("(MFK 2:3)"),
                     btn_mfk_p3 = page_btn("(MFK 3:3)");
static button_data_t btn_app_p1 = page_btn("(APP 1:3)"), btn_app_p2 = page_btn("(APP 2:3)"),
                     btn_app_p3 = page_btn("(APP 3:3)");
static button_data_t btn_key_p1 = page_btn("(KEY 1:2)"), btn_key_p2 = page_btn("(KEY 2:2)");
static button_data_t btn_cw_p1 = page_btn("(CW 1:2)"), btn_cw_p2 = page_btn("(CW 2:2)");
static button_data_t btn_dfn_p1 = page_btn("(DFN 1:3)"), btn_dfn_p2 = page_btn("(DFN 2:3)"),
                     btn_dfn_p3 = page_btn("(DFN 3:3)");
static button_data_t btn_mem_p1 = page_btn("(MEM 1:2)"), btn_mem_p2 = page_btn("(MEM 2:2)");

buttons_page_t        buttons_page_vol_1 = {{&btn_vol_p1, &btn_vol, &btn_sql, &btn_rfg, &btn_tx_pwr}};
static buttons_page_t page_vol_2 = {{&btn_vol_p2, &btn_moni, &btn_pre, &btn_att}};
static buttons_page_t page_mfk_1 = {{&btn_mfk_p1, &btn_rit, &btn_xit, &btn_zoom, &btn_wf_ref}};
static buttons_page_t page_mfk_2 = {{&btn_mfk_p2, &btn_agc, &btn_comp, &btn_if_shift, &btn_vox}};
static buttons_page_t page_mfk_3 = {{&btn_mfk_p3, &btn_wf_mode, &btn_span_wider, &btn_span_narrower, &btn_screen_rec}};

static buttons_page_t page_app_1 = {{&btn_app_p1, &btn_modes, &btn_cluster, &btn_swr, &btn_rec}};
static buttons_page_t page_app_2 = {{&btn_app_p2, &btn_settings, &btn_cwt, &btn_field, &btn_chans}};
static buttons_page_t page_app_3 = {{&btn_app_p3, &btn_bands, &btn_gps, &btn_screen_rec, &btn_cwkbd}};   /* Screen rec: the MFK 3:3 one too */

static buttons_page_t page_key_1 = {{&btn_key_p1, &btn_key_speed, &btn_key_vol, &btn_key_train, &btn_key_tone}};
static buttons_page_t page_key_2 = {{&btn_key_p2, &btn_key_mode, &btn_iambic, &btn_qsk, &btn_ratio}};
static buttons_page_t page_cw_1 = {{&btn_cw_p1, &btn_cw_zap, &btn_cw_peak, &btn_cw_peak_q, &btn_cw_log}};
static buttons_page_t page_cw_2 = {{&btn_cw_p2, &btn_cw_decoder, &btn_cw_tuner, &btn_cw_snr, &btn_cw_parse}};

static buttons_page_t page_dfn_1 = {{&btn_dfn_p1, &btn_dnf, &btn_dnf_center, &btn_dnf_width, &btn_dnf_auto}};
static buttons_page_t page_dfn_2 = {{&btn_dfn_p2, &btn_nb, &btn_nb_level, &btn_nb_width}};
static buttons_page_t page_dfn_3 = {{&btn_dfn_p3, &btn_nr, &btn_nr_level}};

static buttons_page_t page_dfl = {{&btn_flt_low, &btn_flt_high, &btn_flt_bw}};   /* one page, no page button */

static buttons_page_t page_mem_1 = {{&btn_mem_p1, &btn_mem[0], &btn_mem[1], &btn_mem[2], &btn_mem[3]}};
static buttons_page_t page_mem_2 = {{&btn_mem_p2, &btn_mem[4], &btn_mem[5], &btn_mem[6], &btn_mem[7]}};

buttons_group_t buttons_group_gen = {&buttons_page_vol_1, &page_vol_2, &page_mfk_1, &page_mfk_2, &page_mfk_3};
buttons_group_t buttons_group_app = {&page_app_1, &page_app_2, &page_app_3};
buttons_group_t buttons_group_key = {&page_key_1, &page_key_2, &page_cw_1, &page_cw_2};
buttons_group_t buttons_group_dfn = {&page_dfn_1, &page_dfn_2, &page_dfn_3};
buttons_group_t buttons_group_dfl = {&page_dfl};
buttons_group_t buttons_group_vm = {&page_mem_1, &page_mem_2};

static const struct {
    buttons_page_t **pages;
    size_t           n;
} groups[] = {
    {buttons_group_gen, ARRAY_SIZE(buttons_group_gen)}, {buttons_group_app, ARRAY_SIZE(buttons_group_app)},
    {buttons_group_key, ARRAY_SIZE(buttons_group_key)}, {buttons_group_dfn, ARRAY_SIZE(buttons_group_dfn)},
    {buttons_group_dfl, ARRAY_SIZE(buttons_group_dfl)}, {buttons_group_vm, ARRAY_SIZE(buttons_group_vm)},
};

/* ---- the slots on screen ---- */

static void set_state(lv_obj_t *obj, lv_state_t state, bool on) {
    if (on) {
        lv_obj_add_state(obj, state);
    } else {
        lv_obj_clear_state(obj, state);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The knob mark: green VOL or blue MFK for the knob the control is bound to, else the
 * knob a press gives it; "assigned" when it is a bind, "active" when the knob is on it now */
static void marks_refresh(disp_btn_t *s) {
    button_data_t *d    = s->data;
    int            knob = ENCODER_BIND_NONE;
    bool           bound = false;

    if (d && d->ctrl >= 0 && d->encoder_allowed) {
        cfg_ctrl_t ctrl = (cfg_ctrl_t)d->ctrl;
        char       bind = (size_t)ctrl < knob_binds.size() ? knob_binds[ctrl] : ENCODER_BIND_NONE;

        if (bind != ENCODER_BIND_NONE) {
            knob  = bind;
            bound = true;
        } else if (ctrl < CTRL_LAST && d->press == knob_press) {
            knob = vol_by_default(ctrl) ? ENCODER_BIND_VOL : ENCODER_BIND_MFK;
        }
        bool active = (knob == ENCODER_BIND_VOL && ctrl == vol_get_ctrl()) ||
                      (knob == ENCODER_BIND_MFK && ctrl == mfk_ctrl);
        set_state(s->vol_mark, STATE_ACTIVE, active);
        set_state(s->mfk_mark, STATE_ACTIVE, active);
    }
    if (knob != ENCODER_BIND_VOL && knob != ENCODER_BIND_MFK) bound = false;
    set_state(s->vol_mark, STATE_ASSIGNED, bound);
    set_state(s->mfk_mark, STATE_ASSIGNED, bound);
    set_hidden(s->vol_mark, knob != ENCODER_BIND_VOL);
    set_hidden(s->mfk_mark, knob != ENCODER_BIND_MFK);
}

static void label_changed_cb(Subject *subj, void *user_data) {
    button_data_t *d = (button_data_t *)user_data;

    if (d->disp_btn) set_btn_text(d->disp_btn, d->label_fn());
}

static void slot_refresh(disp_btn_t *s) {
    button_data_t *d = s->data;

    if (!d) return;
    switch (d->type) {
        case BTN_TEXT:
            set_btn_text(s, d->label);
            break;
        case BTN_TEXT_FN:
            set_btn_text(s, d->label_fn());
            /* the label follows its setting while shown (slot_clear() ends it) */
            if (d->subj && !d->observer) d->observer = d->subj->subscribe_delayed(label_changed_cb, d);
            break;
        default:
            set_btn_text(s, "");
    }
    set_state(s->parent, LV_STATE_CHECKED, d->mark);
    set_state(s->parent, LV_STATE_DISABLED, d->disabled);
    marks_refresh(s);
}

static void slot_clear(disp_btn_t *s) {
    button_data_t *d = s->data;

    set_btn_text(s, "");
    lv_obj_clear_state(s->parent, LV_STATE_CHECKED | LV_STATE_DISABLED);
    for (lv_obj_t *m : {s->vol_mark, s->mfk_mark}) {
        lv_obj_clear_state(m, STATE_ASSIGNED);
        lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    }
    if (d) {
        d->disp_btn = NULL;
        if (d->observer) {
            d->observer->unsubscribe();
            delete d->observer;
            d->observer = NULL;
        }
        s->data = NULL;
    }
}

static lv_obj_t *make_mark(lv_obj_t *btn, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *m = lv_obj_create(btn);

    lv_obj_set_pos(m, x, y);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(m, &btn_mark_style, 0);
    lv_obj_add_style(m, &btn_mark_assigned_style, STATE_ASSIGNED);
    lv_obj_add_style(m, &btn_mark_active_style, STATE_ACTIVE);
    return m;
}

static void make_slot(disp_btn_t *s, lv_obj_t *parent, lv_coord_t x) {
    lv_obj_t *f = lv_obj_create(parent);

    s->parent = f;
    lv_obj_add_flag(f, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_style_all(f);
    lv_obj_add_style(f, &btn_style, 0);
    lv_obj_add_style(f, &btn_active_style, LV_STATE_CHECKED);
    lv_obj_add_style(f, &btn_disabled_style, LV_STATE_DISABLED);
    /* Drawn buttons: 1 px apart on every side, more when the theme says (btn_gap) */
    lv_obj_set_pos(f, x + BTN_INSET, 480 - BTN_HEIGHT + BTN_INSET);
    lv_obj_set_size(f, BTN_WIDTH - 2 * BTN_INSET, BTN_HEIGHT - 2 * BTN_INSET);

    /* Knob marks: a bar on the left edge, green VOL, blue MFK, or the theme's LED */
    lv_coord_t mx = theme_shape.mark[1] ? theme_shape.mark[0] : 0;
    lv_coord_t my = theme_shape.mark[1] ? (BTN_HEIGHT - 2 - theme_shape.mark[2]) / 2 : theme_shape.deck ? 0 : 10;

    s->vol_mark = make_mark(f, mx, my);
    s->mfk_mark = make_mark(f, mx, my);
    lv_obj_set_style_bg_color(s->mfk_mark, TC(info), 0);
    if (theme_shape.mono) {     /* 1bit: the MFK's mark hollow, the VOL's solid */
        lv_obj_set_style_bg_color(s->mfk_mark, TC(btn), 0);
        lv_obj_set_style_border_width(s->mfk_mark, 1, 0);
        lv_obj_set_style_border_color(s->mfk_mark, TC(text), 0);
    }

    s->label = lv_label_create(f);
    lv_obj_center(s->label);
    lv_obj_set_style_text_align(s->label, LV_TEXT_ALIGN_CENTER, 0);

    s->value = lv_label_create(f);
    lv_obj_set_style_text_font(s->value, &sony_32, 0);
    lv_obj_set_style_text_color(s->value, TC(btn_text), 0);
    lv_obj_align(s->value, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_add_flag(s->value, LV_OBJ_FLAG_HIDDEN);
}

static void knob_binds_cb(Subject *subj, void *user_data) {
    knob_binds = cfg_sm.p_encoder_bind.get();
    for (disp_btn_t &s : slots) slot_refresh(&s);
}

void buttons_init(lv_obj_t *parent) {
    /* No auto notch where the base has none (X6100 boards before revision 3) */
    if (!(radio_caps() & HW_CAP_DNF_AUTO)) page_dfn_1.items[4] = NULL;

    for (const auto &g : groups) {
        for (size_t i = 0; i < g.n; i++) {
            button_data_t *first = g.pages[i]->items[0];

            if (first->press != button_next_page_cb) continue;   /* DFL: one page */
            first->next = g.pages[(i + 1) % g.n];
            first->prev = g.pages[(i + g.n - 1) % g.n];
        }
    }
    for (int i = 0; i < BUTTONS; i++) make_slot(&slots[i], parent, i * BTN_WIDTH);
    cfg_sm.p_encoder_bind.subscribe_delayed_and_notify(knob_binds_cb, NULL);
}

static uint32_t land_index[BUTTONS];   /* their place among the main screen's children */

void buttons_vertical(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    lv_coord_t bw = w - 2 * BTN_INSET, bh = h - 2 * BTN_INSET;

    for (uint8_t i = 0; i < BUTTONS; i++) {
        lv_obj_t           *f = slots[i].parent;
        const lv_img_dsc_t *img;

        land_index[i] = lv_obj_get_index(f);
        lv_obj_set_parent(f, parent);
        lv_obj_set_pos(f, BTN_INSET, i * h + BTN_INSET);
        lv_obj_set_size(f, bw, bh);
        /* the state images at this size too (the normal one is set with the text) */
        if ((img = theme_image(TI_btn_active, bw, bh))) lv_obj_set_style_bg_img_src(f, img, LV_STATE_CHECKED);
        if ((img = theme_image(TI_btn_off, bw, bh))) lv_obj_set_style_bg_img_src(f, img, LV_STATE_DISABLED);
        slot_refresh(&slots[i]);
    }
}

void buttons_horizontal(lv_obj_t *parent) {
    for (uint8_t i = 0; i < BUTTONS; i++) {
        lv_obj_t *f = slots[i].parent;

        lv_obj_set_parent(f, parent);
        lv_obj_move_to_index(f, land_index[i]);
        lv_obj_set_pos(f, i * BTN_WIDTH + BTN_INSET, 480 - BTN_HEIGHT + BTN_INSET);
        lv_obj_set_size(f, BTN_WIDTH - 2 * BTN_INSET, BTN_HEIGHT - 2 * BTN_INSET);
        lv_obj_remove_local_style_prop(f, LV_STYLE_BG_IMG_SRC, LV_STATE_CHECKED);
        lv_obj_remove_local_style_prop(f, LV_STYLE_BG_IMG_SRC, LV_STATE_DISABLED);
        slot_refresh(&slots[i]);
    }
}

void buttons_refresh(button_data_t *data) {
    if (data->disp_btn) slot_refresh(data->disp_btn);
}

void buttons_mark(button_data_t *data, bool val) {
    if (!data) return;
    data->mark = val;
    buttons_refresh(data);
}

void buttons_disabled(button_data_t *data, bool val) {
    data->disabled = val;
    buttons_refresh(data);
}

/* An encoder moved to another control: its LED goes to that button */
void buttons_refresh_marks() {
    for (disp_btn_t &s : slots) {
        if (s.parent) marks_refresh(&s);   /* not before buttons_init() */
    }
}

void buttons_screen_rec_update() {
    buttons_refresh(&btn_screen_rec);
}

void buttons_load(uint8_t n, button_data_t *data) {
    slot_clear(&slots[n]);
    slots[n].data = data;
    if (data) {
        data->disp_btn = &slots[n];
        slot_refresh(&slots[n]);
    }
}

/* All five cleared first: a button on both pages must not be cleared after its new show.
 * No page (an app's page button without one): the buttons stay empty */
void buttons_load_page(buttons_page_t *page) {
    buttons_unload_page();
    if (!page) return;
    cur_page = page;
    for (uint8_t i = 0; i < BUTTONS; i++) buttons_load(i, page->items[i]);
}

void buttons_unload_page() {
    cur_page = NULL;
    for (disp_btn_t &s : slots) slot_clear(&s);
}

void button_next_page_cb(button_data_t *data) {
    buttons_load_page(data->next);
}

void button_prev_page_cb(button_data_t *data) {
    buttons_load_page(data->prev);
}

void buttons_press(uint8_t n, bool hold) {
    button_data_t *d = slots[n].data;

    if (!d || d->disabled) return;
    action_t fn = hold ? d->hold : d->press;
    if (fn) fn(d);
}

/* A group's key: its next page when one of its pages is shown, else its first */
void buttons_load_page_group(buttons_group_t group) {
    for (const auto &g : groups) {
        if (g.pages != group) continue;
        for (size_t i = 0; i < g.n; i++) {
            button_data_t *first = cur_page == g.pages[i] ? cur_page->items[0] : NULL;

            if (first && first->next) {
                first->press(first);
                return;
            }
        }
        buttons_load_page(group[0]);
        return;
    }
}

buttons_page_t *buttons_get_cur_page() {
    return cur_page;
}

/* ---- the text on a button ---- */

/* The button's size as set (the walkie screen's are narrower and taller) */
static lv_coord_t btn_w(disp_btn_t *b) {
    return lv_obj_get_style_width(b->parent, 0);
}

static lv_coord_t btn_h(disp_btn_t *b) {
    return lv_obj_get_style_height(b->parent, 0);
}

static bool btn_narrow(disp_btn_t *b) {
    return btn_w(b) < BTN_WIDTH - 2 * BTN_INSET;
}

/* VISION: the biggest size the label fits in, on one line or two ("Start\nAuto RX") */
static const lv_font_t *vision_label_font(disp_btn_t *b, const char *text) {
    static const lv_font_t *const sizes[] = {&sony_28, &sony_24, &sony_20, &sony_16, &sony_14};
    lv_coord_t                    w = btn_w(b) - 8, h = btn_h(b);
    lv_point_t                    size;

    for (const lv_font_t *f : sizes) {
        lv_txt_get_size(&size, text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= w && size.y <= h) return f;
    }
    return &sony_14;
}

/* The value in a narrow button (walkie): the biggest size that fits its width ("3200 Hz") */
static const lv_font_t *narrow_value_font(disp_btn_t *b, const char *text) {
    static const lv_font_t *const sizes[] = {&sony_32, &sony_28, &sony_24, &sony_20};
    lv_point_t                    size;

    for (const lv_font_t *f : sizes) {
        lv_txt_get_size(&size, text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= btn_w(b) - 8) return f;
    }
    return &sony_18;
}

/* "Name:\nvalue" shows a small name over a large value; "(PAGE 1:2)" is a
 * page button, drawn darker; anything else is an action, centred. */
static void set_btn_text(disp_btn_t *b, const char *text) {
    const char *nl = strchr(text, '\n');

    if (nl && nl > text && nl[-1] == ':') {
        char name[32];
        snprintf(name, sizeof(name), "%.*s", (int)(nl - text - 1), text);
        /* Deck: "Volume 30" on one line, the name grey, the value white */
        if (theme_shape.deck && !btn_narrow(b)) {
            /* The biggest that fits on one line */
            static const lv_font_t *const fonts[] = {&sony_30, &sony_28, &sony_26, &sony_24, &sony_20};
            const lv_font_t *f;
            lv_point_t       ns, vs;

            for (size_t i = theme_kx3 ? 3 : 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {   /* Plus: low */
                f = fonts[i];
                lv_txt_get_size(&ns, name, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                lv_txt_get_size(&vs, nl + 1, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (ns.x + 6 + vs.x <= btn_w(b) - 8) break;
            }
            lv_label_set_text(b->label, name);
            lv_obj_set_style_text_font(b->label, f, 0);
            lv_obj_set_style_text_color(b->label, TC(text_dim), 0);
            lv_obj_align(b->label, LV_ALIGN_CENTER, -(vs.x + 6) / 2, 0);
            lv_label_set_text(b->value, nl + 1);
            lv_obj_set_style_text_font(b->value, f, 0);
            lv_obj_align(b->value, LV_ALIGN_CENTER, (ns.x + 6) / 2, 0);
            lv_obj_clear_flag(b->value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(b->parent, TC(btn), 0);
            return;
        }
        /* Name over value; one size smaller when the two lines don't fit (taller fonts, btn_gap) */
        bool fit = lv_font_get_line_height(&sony_22) + lv_font_get_line_height(&sony_32) + 9
                   <= btn_h(b);
        lv_label_set_text(b->label, name);
        lv_obj_set_style_text_font(b->label, theme_shape.vision || btn_narrow(b) ? &sony_18 : fit ? &sony_22 : &sony_20, 0);
        lv_obj_set_style_text_color(b->label, TC(text_head), 0);
        lv_obj_align(b->label, LV_ALIGN_TOP_MID, 0, theme_shape.vision ? 0 : fit ? 4 : 2);
        /* VISION: the value as big as the button takes, the name over it (capitals and digits: no overlap) */
        lv_obj_set_style_text_font(b->value, btn_narrow(b)        ? narrow_value_font(b, nl + 1)
                                             : theme_shape.vision ? &sony_30
                                             : fit                ? &sony_32 : &sony_28, 0);
        lv_obj_align(b->value, LV_ALIGN_BOTTOM_MID, 0, theme_shape.vision ? 0 : fit ? -5 : -2);
        lv_label_set_text(b->value, nl + 1);
        lv_obj_clear_flag(b->value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(b->parent, TC(btn), 0);
        return;
    }

    bool page = text[0] == '(';
    if (page) {
        char   t[32];
        size_t n = 0;
        for (const char *c = text; *c && n < sizeof(t) - 1; c++)
            if (*c != '(' && *c != ')') t[n++] = *c;
        t[n] = '\0';
        lv_label_set_text(b->label, t);
    } else {
        lv_label_set_text(b->label, text);
    }
    lv_obj_set_style_text_font(b->label, theme_shape.vision || btn_narrow(b) ? vision_label_font(b, lv_label_get_text(b->label))
                                                                              : theme_kx3 ? &sony_24
                                                                              : theme_shape.deck ? &sony_30 : page ? &sony_28 : &sony_30, 0);
    if (page) lv_obj_set_style_text_color(b->label, TC(btn_page_text), 0);
    else lv_obj_remove_local_style_prop(b->label, LV_STYLE_TEXT_COLOR, 0);   /* the button's: btn_text, btn_active_text, btn_off_text */
    lv_obj_center(b->label);
    lv_obj_add_flag(b->value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(b->parent, page ? TC(btn_page) : TC(btn), 0);
    /* Themes with images: the page button's own, else the button's */
    const lv_img_dsc_t *img = page ? theme_image(TI_btn_page, btn_w(b), btn_h(b)) : NULL;
    if (!img) img = theme_image(TI_btn, btn_w(b), btn_h(b));
    if (img) lv_obj_set_style_bg_img_src(b->parent, img, 0);
}
