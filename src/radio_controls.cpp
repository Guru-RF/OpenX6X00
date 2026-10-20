/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* What the VOL and MFK knobs change, and the button actions that toggle the same settings.
 * A turn changes the setting and writes "Name: value" for the message line. */

#include "radio_controls.h"

#include "cw.h"
#include "knob_info.h"
#include "util.h"
#include "util.hpp"

#include "cfg/settings_manager.h"

extern "C" {
#include "cluster/client.h"
#include "cluster_column.h"
#include "msg_box.h"
#include "rtty.h"
}

using FloatParam = Parameter<float, int32_t, 10>;

/* Settings turned in steps; the value is aligned to the step even when not turned */
static const struct {
    cfg_ctrl_t          ctrl;
    Parameter<int32_t> *p;
    int32_t             step;
    const char         *fmt;
} int_steps[] = {
    {CTRL_RFG, &cfg_sm.p_rfgain, 1, "RF gain: %i"},
    {CTRL_WEFAX_ALIGN, &cfg_sm.p_wefax_align, 1, "WEFAX Align: %i"},     /* WEFAX app: picture shift */
    {CTRL_WEFAX_TILT, &cfg_sm.p_wefax_tilt, 1, "WEFAX Tilt: %i"},        /* and slant */
    {CTRL_SQL, &cfg_sm.p_squelch, 1, "Voice SQL: %i"},
    {CTRL_MONI, &cfg_sm.p_moni, 1, "Moni level: %i"},
    {CTRL_KEY_SPEED, &cfg_sm.p_key_speed, 1, "Key speed: %i wpm"},
    {CTRL_KEY_TONE, &cfg_sm.p_key_tone, 10, "Key tone: %i Hz"},
    {CTRL_KEY_VOL, &cfg_sm.p_key_vol, 1, "Key volume: %i"},
    {CTRL_QSK_TIME, &cfg_sm.p_qsk_time, 10, "QSK time: %i ms"},
    {CTRL_CW_PEAK_Q, &cfg_sm.p_cw_peak_q, 1, "CW peak Q: %i"},
    {CTRL_RIT, &cfg_sm.p_rit, 10, "RIT: %+i"},
    {CTRL_XIT, &cfg_sm.p_xit, 10, "XIT: %+i"},
    {CTRL_DNF_CENTER, &cfg_sm.p_dnf_center, 50, "DNF center: %i Hz"},
    {CTRL_DNF_WIDTH, &cfg_sm.p_dnf_width, 5, "DNF width: %i Hz"},
    {CTRL_NB_LEVEL, &cfg_sm.p_nb_level, 5, "NB level: %i"},
    {CTRL_NB_WIDTH, &cfg_sm.p_nb_width, 5, "NB width: %i"},
    {CTRL_NR_LEVEL, &cfg_sm.p_nr_level, 5, "NR level: %i"},
    {CTRL_WF_REF, &cfg_sm.p_waterfall_ref, 1, "Waterfall ref: %+i dB"},
};

static const struct {
    cfg_ctrl_t  ctrl;
    FloatParam *p;
    const char *fmt;
} float_steps[] = {     /* all in steps of 0.1 */
    {CTRL_PWR, &cfg_sm.p_pwr, "Power: %0.1f W"},
    {CTRL_KEY_RATIO, &cfg_sm.p_key_ratio, "Key ratio: %0.1f"},
    {CTRL_CW_DECODER_SNR, &cfg_sm.p_cw_decoder_snr, "CW decoder SNR: %0.1f dB"},
};

/* The filter edges follow the mode: changed only when turned. Step 0: by the mode */
static const struct {
    cfg_ctrl_t                  ctrl;
    ComputedParameter<int32_t> *p;
    int32_t                     step;
    const char                 *fmt;
} filter_steps[] = {
    {CTRL_FILTER_LOW, &cfg_sm.cp_cur_filter_low, 10, "Filter low: %i Hz"},
    {CTRL_FILTER_HIGH, &cfg_sm.cp_cur_filter_high, 0, "Filter high: %i Hz"},
    {CTRL_FILTER_BW, &cfg_sm.cp_cur_filter_bw, 20, "Filter bw: %i Hz"},
};

/* On/off settings: any turn flips them */
static const struct {
    cfg_ctrl_t          ctrl;
    Parameter<int32_t> *p;
    const char         *fmt;
    const char         *off, *on;
} toggles[] = {
    {CTRL_VOX_ON, &cfg_sm.p_vox_en, "VOX: %s", "Off", "On"},
    {CTRL_KEY_TRAIN, &cfg_sm.p_key_train, "Key train: %s", "Off", "On"},
    {CTRL_CW_PEAK_ON, &cfg_sm.p_cw_peak_on, "CW peak: %s", "Off", "On"},
    {CTRL_DNF, &cfg_sm.p_dnf, "DNF: %s", "Off", "On"},
    {CTRL_DNF_AUTO, &cfg_sm.p_dnf_auto, "DNF auto: %s", "Off", "On"},
    {CTRL_NB, &cfg_sm.p_nb, "NB: %s", "Off", "On"},
    {CTRL_NR, &cfg_sm.p_nr, "NR: %s", "Off", "On"},
    {CTRL_CW_DECODER, &cfg_sm.p_cw_decoder, "CW decoder: %s", "Off", "On"},
    {CTRL_CW_TUNE, &cfg_sm.p_cw_tune, "CW tune: %s", "Off", "On"},
    {CTRL_WF_MODE, &cfg_sm.p_wf_mode, "Waterfall: %s", "Centered", "Fixed"},
    {CTRL_CLUSTER_ORDER, &cfg_sm.p_cluster_order, "Cluster order: %s", "frequency", "newest first"},
    {CTRL_CLUSTER_BANDS, &cfg_sm.p_cluster_all_bands, "Cluster: %s", "current band", "all bands"},
};

template <typename T, typename D, int32_t S>
static T turn(Parameter<T, D, S> &p, int32_t diff, T step) {
    p.set(align(p.get() + diff * step, step));
    return p.get();     /* as the setting took it */
}

static int32_t filter_high_step() {
    int32_t mode = cfg_sm.cp_cur_mode.get();

    return mode == x6100_mode_cw || mode == x6100_mode_cwr ? 10 : 50;
}

/* One step round a setting of n values (0 .. n-1) */
static int32_t cycle(Parameter<int32_t> &p, int32_t n, int32_t diff) {
    int32_t v = p.get();

    if (diff) {
        v = (v + (diff > 0 ? 1 : n - 1)) % n;
        p.set(v);
    }
    return v;
}

static bool from_table(cfg_ctrl_t ctrl, int32_t diff, char *buf, size_t size) {
    for (auto &s : int_steps) {
        if (s.ctrl == ctrl) {
            snprintf(buf, size, s.fmt, turn(*s.p, diff, s.step));
            return true;
        }
    }
    for (auto &s : float_steps) {
        if (s.ctrl == ctrl) {
            snprintf(buf, size, s.fmt, turn(*s.p, diff, 0.1f));
            return true;
        }
    }
    for (auto &s : filter_steps) {
        if (s.ctrl == ctrl) {
            int32_t v = s.p->get();

            if (diff) {
                int32_t step = s.step ? s.step : filter_high_step();

                s.p->set(align(v + diff * step, step));
                v = s.p->get();
            }
            snprintf(buf, size, s.fmt, v);
            return true;
        }
    }
    for (auto &t : toggles) {
        if (t.ctrl == ctrl) {
            bool on = t.p->get();

            if (diff) {
                on = !on;
                t.p->set(on);
            }
            snprintf(buf, size, t.fmt, on ? t.on : t.off);
            return true;
        }
    }
    return false;
}

/* IF shift needs base firmware 1.1.9 (or a board revision 8 and later) */
static bool if_shift_supported() {
    x6100_base_ver_t ver = x6100_control_get_base_ver();

    return util_compare_version(ver, (x6100_base_ver_t){1, 1, 9, 0}) >= 0 || ver.rev >= 8;
}

void controls_encoder_update(cfg_ctrl_t ctrl, int32_t diff, std::string &msg) {
    char  *buf  = msg.data();
    size_t size = msg.capacity();
    int32_t i;

    if (from_table(ctrl, diff, buf, size)) {
        return;
    }
    switch (ctrl) {
        case CTRL_VOL:
            snprintf(buf, size, "Volume: %i", radio_change_vol(diff));
            break;

        case CTRL_SPECTRUM_FACTOR:      /* zoom x1 x2 x4 x8: a step doubles or halves it */
            i = cfg_sm.p_mode_zoom.get();
            if (diff) {
                cfg_sm.p_mode_zoom.set(diff > 0 ? i << diff : i >> -diff);
                i = cfg_sm.p_mode_zoom.get();
            }
            snprintf(buf, size, "Zoom: x%i", i);
            break;

        case CTRL_COMP:
            snprintf(buf, size, "Compressor ratio: %s", params_comp_str_get(turn(cfg_sm.p_comp, diff, 1)));
            break;

        case CTRL_KEY_MODE:     /* manual, auto left, auto right */
            i = cycle(cfg_sm.p_key_mode, x6100_key_auto_right + 1, diff);
            snprintf(buf, size, "Key mode: %s", params_key_mode_str_get((x6100_key_mode_t)i));
            break;

        case CTRL_IAMBIC_MODE:  /* A, B */
            i = cycle(cfg_sm.p_iambic_mode, x6100_iambic_b + 1, diff);
            snprintf(buf, size, "Iambic mode: %s", params_iambic_mode_str_ger((x6100_iambic_mode_t)i));
            break;

        case CTRL_RTTY_RATE:
            snprintf(buf, size, "RTTY rate: %0.2f", rtty_change_rate(diff));
            break;

        case CTRL_RTTY_SHIFT:
            snprintf(buf, size, "RTTY shift: %i Hz", rtty_change_shift(diff));
            break;

        case CTRL_RTTY_CENTER:
            snprintf(buf, size, "RTTY center: %i Hz", rtty_change_center(diff));
            break;

        case CTRL_RTTY_REVERSE:
            snprintf(buf, size, "RTTY reverse: %s", rtty_change_reverse(diff) ? "On" : "Off");
            break;

        case CTRL_CLUSTER:
            snprintf(buf, size, "Cluster: %s", cluster_column_move(diff));
            knobs_set_mfk_param(CTRL_CLUSTER);  /* no subject: refresh the knob label */
            break;

        case CTRL_CLUSTER_MODES: {  /* the modes filter of Settings > Cluster, in five presets */
            static const uint8_t      presets[] = {CLUSTER_FM_ALL, CLUSTER_FM_CW, CLUSTER_FM_SSB, CLUSTER_FM_DIG,
                                                   CLUSTER_FM_CW | CLUSTER_FM_DIG};
            static const char *const  names[]   = {"All", "CW", "SSB", "DIG", "CW+DIG"};
            const cluster_filter_t   *f         = cluster_filter();
            int                       cur       = 0;

            for (int k = 0; k < 5; k++)
                if (f->modes == presets[k]) cur = k;
            if (diff) {
                cur = (cur + (diff > 0 ? 1 : 4)) % 5;
                cluster_filter_save(presets[cur], f->bands, f->activity);
            }
            snprintf(buf, size, "Cluster modes: %s", names[cur]);
            knobs_set_mfk_param(CTRL_CLUSTER_MODES);
            break;
        }

        case CTRL_IF_SHIFT:
            if (if_shift_supported()) {
                snprintf(buf, size, "IF shift: %i Hz", turn(cfg_sm.p_band_if_shift, diff, 100));
            } else {
                msg_update_text_fmt("IF shift is not supported");
            }
            break;

        default:
            break;
    }
}

/* The next control (dir > 0) or the one before bound to this knob, round the list;
 * dir 0: this one if it is bound, else the next. The cluster items are on the MFK while
 * the cluster column is shown, whatever the binds say */
cfg_ctrl_t controls_encoder_get_next(encoder_binds_t encoder, cfg_ctrl_t current, int16_t dir) {
    std::string binds = cfg_sm.p_encoder_bind.get();
    size_t      n     = binds.size();

    auto bound = [&](size_t c) {
        if (c == CTRL_CLUSTER || c == CTRL_CLUSTER_ORDER || c == CTRL_CLUSTER_BANDS || c == CTRL_CLUSTER_MODES) {
            return encoder == ENCODER_BIND_MFK && cfg_sm.p_cluster_column.get();
        }
        return binds[c] == encoder;
    };

    if (dir == 0) {
        if (current < n && bound(current)) {
            return current;
        }
        dir = 1;
    }
    for (size_t k = 1; k <= n; k++) {
        size_t c = (dir > 0 ? current + k : 2 * n + current - k) % n;

        if (bound(c)) {
            return (cfg_ctrl_t)c;
        }
    }
    LV_LOG_ERROR("No next/prev control for %c", encoder);
    return current;
}

/* Buttons */

static void flip(Parameter<int32_t> &p) {
    p.set(!p.get());
}

void controls_toggle_key_train(button_data_t *data) {
    flip(cfg_sm.p_key_train);
}

void controls_toggle_key_iambic_mode(button_data_t *data) {
    cfg_sm.p_iambic_mode.set(cfg_sm.p_iambic_mode.get() == x6100_iambic_a ? x6100_iambic_b : x6100_iambic_a);
}

void controls_toggle_cw_decoder(button_data_t *data) {
    flip(cfg_sm.p_cw_decoder);
}

void controls_toggle_cw_tuner(button_data_t *data) {
    flip(cfg_sm.p_cw_tune);
}

void controls_toggle_cw_peak(button_data_t *data) {
    flip(cfg_sm.p_cw_peak_on);
}

void controls_toggle_dnf(button_data_t *data) {
    flip(cfg_sm.p_dnf);
}

void controls_toggle_dnf_auto(button_data_t *data) {
    flip(cfg_sm.p_dnf_auto);
}

void controls_toggle_nb(button_data_t *data) {
    flip(cfg_sm.p_nb);
}

void controls_toggle_nr(button_data_t *data) {
    flip(cfg_sm.p_nr);
}

void controls_toggle_vox(button_data_t *data) {
    flip(cfg_sm.p_vox_en);
}

void controls_toggle_wf_mode(button_data_t *data) {
    flip(cfg_sm.p_wf_mode);
}

/* Span +/- buttons: wider span = lower zoom (1/2/4/8) */
void controls_span_wider(button_data_t *data) {
    cfg_sm.p_mode_zoom.set(cfg_sm.p_mode_zoom.get() >> 1);
}

void controls_span_narrower(button_data_t *data) {
    cfg_sm.p_mode_zoom.set(cfg_sm.p_mode_zoom.get() << 1);
}

/* CW ZAP: tune so that the signal the decoder hears sits on the key tone, to 10 Hz */
void controls_cw_zap(button_data_t *data) {
    int32_t mode = cfg_sm.cp_cur_mode.get();

    if (mode != x6100_mode_cw && mode != x6100_mode_cwr) {
        return;
    }

    float   tone_freq = cw_get_tone_freq();
    int32_t key_tone  = cfg_sm.p_key_tone.get();
    int32_t freq      = cfg_sm.cp_fg_freq.get();

    LV_LOG_USER("tone_freq: %f, key_tone: %i", tone_freq, key_tone);
    if (mode == x6100_mode_cw) {
        freq += tone_freq - key_tone;
    } else {
        freq -= tone_freq - key_tone;
    }
    cfg_sm.cp_fg_freq.set((freq + 5) / 10 * 10);
}

/* 40 -> "40m", "40m CW", "40m SSB", not "4m" */
static bool band_matches(const std::string &name, int32_t m) {
    char pre[8];
    int  l = snprintf(pre, sizeof(pre), "%im", m);
    return name.compare(0, l, pre) == 0 && (name.size() == (size_t)l || name[l] == ' ');
}

void controls_set_band_m(int32_t m) {
    BandInfoLoadResult cur = BandsTable::get_by_id(cfg_sm.current_band_id());

    if (cur.rc == SUCCESS && band_matches(cur.value.name, m))
        return;
    int32_t  id    = BAND_UNDEFINED;
    uint32_t start = UINT32_MAX;
    for (const BandInfo &b : BandsTable::all_bands()) {
        if (b.type != BAND_ACTIVE || !band_matches(b.name, m) || !cfg_sm.is_valid_hw_freq(b.start_freq))
            continue;
        if (b.start_freq < start) {
            id    = b.id;
            start = b.start_freq;
        }
    }
    if (id != BAND_UNDEFINED)
        cfg_sm.p_band_id.set(id);
}
