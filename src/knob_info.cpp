/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The two lines over the buttons that say what VOL and MFK control and its value (Settings >
 * Knob info). The name is bright while the knob chooses the control, the value while it
 * changes the value. */

#include "knob_info.h"

#include "button_bar.h"
#include "cluster/client.h"
#include "cluster_column.h"
#include "mfk_control.h"
#include "vol_control.h"

#include "cfg/settings_manager.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "gui_styles.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
}

#define LINE_H 26
#define LEFT   2

/* A control as the label shows it: its name and its value now */
struct Knob {
    const char                   *name;
    Subject                      *subj;     /* nullptr: the cluster column (controls.cpp refreshes it) */
    std::function<std::string()>  value;
};

static Knob num(const char *name, SubjectT<int32_t> &s) {
    return {name, &s, [&s] { return std::to_string(s.get()); }};
}

static Knob num(const char *name, SubjectT<float> &s) {
    return {name, &s, [&s] {
        char buf[16];

        snprintf(buf, sizeof(buf), "%0.1f", s.get());
        return std::string(buf);
    }};
}

static Knob pick(const char *name, SubjectT<int32_t> &s, std::vector<std::string> names) {
    return {name, &s, [&s, names] {
        int32_t v = s.get();

        return v >= 0 && (size_t)v < names.size() ? names[v] : std::string("Unknown");
    }};
}

static Knob on_off(const char *name, SubjectT<int32_t> &s) {
    return pick(name, s, {"Off", "On"});
}

/* The controls of the app knobs (RTTY, WEFAX) have none: those apps show their own */
static const std::map<int, Knob> knobs = {
    {CTRL_VOL, num("Volume", cfg_sm.p_volume)},
    {CTRL_SQL, num("Voice SQL", cfg_sm.p_squelch)},
    {CTRL_RFG, num("RF gain", cfg_sm.p_rfgain)},
    {CTRL_FILTER_LOW, num("Filter low", cfg_sm.cp_cur_filter_low)},
    {CTRL_FILTER_HIGH, num("Filter high", cfg_sm.cp_cur_filter_high)},
    {CTRL_FILTER_BW, num("Filter bw", cfg_sm.cp_cur_filter_bw)},
    {CTRL_PWR, num("Power", cfg_sm.p_pwr)},
    {CTRL_MONI, num("Moni level", cfg_sm.p_moni)},
    {CTRL_SPECTRUM_FACTOR, num("Zoom", cfg_sm.p_mode_zoom)},
    {CTRL_COMP, {"Compressor", &cfg_sm.p_comp, [] { return std::string(params_comp_str_get(cfg_sm.p_comp.get())); }}},
    {CTRL_VOX_ON, on_off("VOX", cfg_sm.p_vox_en)},
    {CTRL_RIT, num("RIT", cfg_sm.p_rit)},
    {CTRL_XIT, num("XIT", cfg_sm.p_xit)},
    {CTRL_IF_SHIFT, num("IF shift", cfg_sm.p_band_if_shift)},
    {CTRL_WF_REF, num("WF ref", cfg_sm.p_waterfall_ref)},
    {CTRL_WF_MODE, pick("WF mode", cfg_sm.p_wf_mode, {"Centered", "Fixed"})},
    {CTRL_CLUSTER, {"Cluster", nullptr, [] { return std::string(cluster_column_move(0)); }}},
    {CTRL_CLUSTER_ORDER, pick("Cluster order", cfg_sm.p_cluster_order, {"Freq", "Time"})},
    {CTRL_CLUSTER_BANDS, pick("Cluster bands", cfg_sm.p_cluster_all_bands, {"Band", "All"})},
    {CTRL_CLUSTER_MODES, {"Cluster modes", nullptr, [] {
        uint8_t m = cluster_filter()->modes;
        return std::string(m == CLUSTER_FM_ALL ? "All" : m == CLUSTER_FM_CW ? "CW" : m == CLUSTER_FM_SSB ? "SSB"
                           : m == CLUSTER_FM_DIG ? "DIG" : m == (CLUSTER_FM_CW | CLUSTER_FM_DIG) ? "CW+DIG" : "Some");
    }}},

    {CTRL_DNF, on_off("Notch filter", cfg_sm.p_dnf)},
    {CTRL_DNF_CENTER, num("DNF center", cfg_sm.p_dnf_center)},
    {CTRL_DNF_WIDTH, num("DNF width", cfg_sm.p_dnf_width)},
    {CTRL_DNF_AUTO, on_off("DNF auto", cfg_sm.p_dnf_auto)},
    {CTRL_NB, on_off("Noise blanker", cfg_sm.p_nb)},
    {CTRL_NB_LEVEL, num("NB level", cfg_sm.p_nb_level)},
    {CTRL_NB_WIDTH, num("NB width", cfg_sm.p_nb_width)},
    {CTRL_NR, on_off("Noise reduction", cfg_sm.p_nr)},
    {CTRL_NR_LEVEL, num("NR level", cfg_sm.p_nr_level)},

    {CTRL_KEY_SPEED, num("Key speed", cfg_sm.p_key_speed)},
    {CTRL_KEY_TRAIN, on_off("Key train", cfg_sm.p_key_train)},
    {CTRL_KEY_MODE, pick("Key mode", cfg_sm.p_key_mode, {"Manual", "Auto-L", "Auto-R"})},
    {CTRL_IAMBIC_MODE, pick("Iambic mode", cfg_sm.p_iambic_mode, {"A", "B"})},
    {CTRL_KEY_TONE, num("Key tone", cfg_sm.p_key_tone)},
    {CTRL_KEY_VOL, num("Key vol", cfg_sm.p_key_vol)},
    {CTRL_QSK_TIME, num("QSK time", cfg_sm.p_qsk_time)},
    {CTRL_KEY_RATIO, num("Key ratio", cfg_sm.p_key_ratio)},
    {CTRL_CW_DECODER, on_off("CW decoder", cfg_sm.p_cw_decoder)},
    {CTRL_CW_TUNE, on_off("CW tuner", cfg_sm.p_cw_tune)},
    {CTRL_CW_DECODER_SNR, num("CW decoded snr", cfg_sm.p_cw_decoder_snr)},
    {CTRL_CW_PEAK_ON, on_off("CW peak", cfg_sm.p_cw_peak_on)},
    {CTRL_CW_PEAK_Q, num("CW peak Q", cfg_sm.p_cw_peak_q)},
};

/* One of the two lines */
class KnobLine {
    lv_obj_t   *label = nullptr;   /* made again by knobs_init() on a theme change */
    const char *arrow;
    const Knob *knob = nullptr;
    bool        edit = true;
    Subscription sub;

    static void changed_cb(Subject *subj, void *self) {
        ((KnobLine *)self)->show();
    }

  public:
    explicit KnobLine(const char *arrow) : arrow(arrow) {}

    lv_obj_t *obj() {
        return label;
    }

    void show() {
        if (!knob || !label) {
            return;
        }

        std::string val = knob->value();
        uint32_t    on = theme_hex(TC_btn_mark), off = theme_hex(TC_text_soft);
        char        buf[64];

        snprintf(buf, sizeof(buf), "%s #%06x %s:# #%06x %s#", arrow, edit ? off : on, knob->name, edit ? on : off,
                 val.c_str());
        lv_label_set_text(label, buf);
    }

    void make(lv_obj_t *parent, lv_coord_t y) {
        label = lv_label_create(parent);
        lv_obj_add_style(label, &knobs_style, 0);
        lv_obj_set_pos(label, LEFT, y);
        lv_label_set_recolor(label, true);
        lv_label_set_text(label, "");
        set_edit(true);
    }

    void set_edit(bool on) {
        edit = on;
        show();
    }

    void set_knob(const Knob *k) {
        if (k == knob) {
            show();
            return;
        }
        knob = k;

        ObserverDelayed *obs = k->subj ? k->subj->subscribe_delayed(changed_cb, this) : nullptr;

        sub = Subscription(obs);
        if (obs) {
            sub->notify();      /* shown from the GUI loop, as any change */
        } else {
            show();
        }
    }

    /* For an app that draws the knobs itself; ctrl gives the scale */
    bool info(cfg_ctrl_t ctrl, const char **name, char *val, size_t size, float *level, bool *is_edit) {
        if (!knob) {
            return false;
        }
        *name = knob->name;
        snprintf(val, size, "%s", knob->value().c_str());
        *is_edit = edit;

        /* the settings' ranges (settings_manager.h) */
        float max = ctrl == CTRL_VOL                                            ? 55
                  : ctrl == CTRL_SQL || ctrl == CTRL_RFG || ctrl == CTRL_MONI ? 100
                  : ctrl == CTRL_PWR                                            ? 10
                                                                                : 0;

        *level = max > 0 ? atof(val) / max : -1;
        return true;
    }
};

/* Never destroyed: their subscriptions must not outlive the settings at exit */
static KnobLine &vol_line = *new KnobLine(LV_SYMBOL_UP);
static KnobLine &mfk_line = *new KnobLine(LV_SYMBOL_DOWN);
static bool      enabled;

static void knob_info_cb(Subject *subj, void *user_data) {
    enabled = cfg_sm.p_knob_info.get();
}

void knobs_init(lv_obj_t *parent) {
    lv_coord_t y = 480 - BTN_HEIGHT - 5;     /* just over the buttons */

    vol_line.make(parent, y - LINE_H * 2);
    mfk_line.make(parent, y - LINE_H);
    cfg_sm.p_knob_info.subscribe_delayed_and_notify(knob_info_cb, nullptr);
}

void knobs_display(bool on) {
    for (lv_obj_t *l : {vol_line.obj(), mfk_line.obj()}) {
        if (on && enabled) {
            lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

bool knobs_info(bool vol, const char **name, char *val, size_t size, float *level, bool *edit) {
    return vol ? vol_line.info(vol_get_ctrl(), name, val, size, level, edit)
               : mfk_line.info(mfk_ctrl, name, val, size, level, edit);
}

bool knobs_visible() {
    return vol_line.obj() && !lv_obj_has_flag(vol_line.obj(), LV_OBJ_FLAG_HIDDEN);
}

static void set_param(KnobLine &line, cfg_ctrl_t ctrl, const char *knob) {
    auto k = knobs.find(ctrl);

    if (k == knobs.end()) {
        LV_LOG_WARN("%s control %d has no knob line, skip", knob, ctrl);
        return;
    }
    line.set_knob(&k->second);
    buttons_refresh_marks();
}

void knobs_set_vol_state(bool edit) {
    vol_line.set_edit(edit);
}

void knobs_set_vol_param(cfg_ctrl_t control) {
    set_param(vol_line, control, "VOL");
}

void knobs_set_mfk_state(bool edit) {
    mfk_line.set_edit(edit);
}

void knobs_set_mfk_param(cfg_ctrl_t control) {
    set_param(mfk_line, control, "MFK");
}
