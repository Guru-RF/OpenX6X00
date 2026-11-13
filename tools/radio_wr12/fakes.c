/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of radio.c's side of the X6200's WR12 (#134): the GUI around the real src/radio.c, as little
 *  of it as radio.c needs. The settings (subjects that notify in the order they were subscribed in, the dial computed
 *  from the VFOs as the settings manager does, the rules of where the radio receives and transmits as
 *  settings_manager.h has them, cfg_tx_refusal as settings_manager_api.cpp has it over the real ham_bands.c), the
 *  GUI thread's queue, the screen's messages, the clock, a stand-in WR12 driver, and what reaches the base on its way
 *  to the real hw_stub.c. The rest answers as an idle radio would. See fakes.h.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#define _GNU_SOURCE
#include "fakes.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backlight.h"
#include "base_guard.h"
#include "bat_est.h"
#include "bat_watch.h"
#include "btaudio.h"
#include "cfg/ham_bands.h"
#include "clock.h"
#include "cw.h"
#include "cw_encoder.h"
#include "dialog_msg_voice.h"
#include "dialog_swrscan.h"
#include "dsp.h"
#include "freq_strip.h"
#include "front_keys.h"
#include "hkey.h"
#include "hw/hw.h"
#include "msg.h"
#include "paddle_keyer.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_rec.h"
#include "settings_window.h"
#include "smeter.h"
#include "start_trace.h"
#include "swr_guard.h"
#include "tune_tx.h"
#include "tx_info.h"
#include "util.h"
#include "wifi.h"
#include "wr12.h"

/* ---- The settings ------------------------------------------------------------------------------------------------ */

#define MAX_OBS 24

typedef struct fake_subj {
    const char *name;
    int32_t     i;
    float       f;
    struct {
        observer_cb fn;
        void       *ud;
    } obs[MAX_OBS];
    int         n_obs;
    int32_t   (*compute)(void);         /* a computed one: its value from its sources */
    void      (*reverse)(int32_t);      /* and a set of it, onto its sources */
    bool        updating;
} fake_subj_t;

/* Every setting radio.c reads, as cfg_api.h declares it */
#define SETTINGS \
    X(ParamInt, cfg_agc_hang) \
    X(ParamInt, cfg_agc_knee) \
    X(ParamInt, cfg_agc_slope) \
    X(ParamInt, cfg_atu_enabled) \
    X(ParamInt, cfg_band_current_vfo) \
    X(ParamFloat, cfg_band_dac_offset) \
    X(ParamInt, cfg_band_id) \
    X(ParamInt, cfg_band_if_shift) \
    X(ParamInt, cfg_band_split) \
    X(ParamInt, cfg_band_tx_i_offset) \
    X(ParamInt, cfg_band_tx_q_offset) \
    X(ParamInt, cfg_band_tx_shift) \
    X(ParamInt, cfg_band_vfoa_agc) \
    X(ParamInt, cfg_band_vfoa_att) \
    X(ParamInt, cfg_band_vfoa_freq) \
    X(ParamInt, cfg_band_vfoa_mode) \
    X(ParamInt, cfg_band_vfoa_pre) \
    X(ParamInt, cfg_band_vfob_agc) \
    X(ParamInt, cfg_band_vfob_att) \
    X(ParamInt, cfg_band_vfob_freq) \
    X(ParamInt, cfg_band_vfob_mode) \
    X(ParamInt, cfg_band_vfob_pre) \
    X(ComputedParamInt, cfg_bg_freq) \
    X(ParamInt, cfg_cessb_on) \
    X(ParamFloat, cfg_cessb_power_up) \
    X(ParamInt, cfg_comp) \
    X(ParamFloat, cfg_comp_makeup_offset) \
    X(ParamFloat, cfg_comp_threshold_offset) \
    X(ComputedParamInt, cfg_cur_agc) \
    X(ComputedParamInt, cfg_cur_filter_high) \
    X(ComputedParamInt, cfg_cur_filter_low) \
    X(ComputedParamInt, cfg_cur_mode) \
    X(ParamInt, cfg_cw_decoder) \
    X(ParamInt, cfg_cw_peak_on) \
    X(ParamInt, cfg_cw_peak_q) \
    X(ParamInt, cfg_dnf) \
    X(ParamInt, cfg_dnf_auto) \
    X(ParamInt, cfg_dnf_center) \
    X(ParamInt, cfg_dnf_width) \
    X(ComputedParamInt, cfg_fg_freq) \
    X(ParamInt, cfg_fm_emphasis) \
    X(ParamInt, cfg_hmic) \
    X(ParamInt, cfg_iambic_mode) \
    X(ParamInt, cfg_imic) \
    X(ParamInt, cfg_key_mode) \
    X(ParamFloat, cfg_key_ratio) \
    X(ParamInt, cfg_key_speed) \
    X(ParamInt, cfg_key_tone) \
    X(ParamInt, cfg_key_train) \
    X(ParamInt, cfg_key_vol) \
    X(ParamInt, cfg_mic) \
    X(ParamInt, cfg_mode_zoom) \
    X(ParamInt, cfg_moni) \
    X(ParamInt, cfg_nb) \
    X(ParamInt, cfg_nb_level) \
    X(ParamInt, cfg_nb_width) \
    X(ParamInt, cfg_nr) \
    X(ParamInt, cfg_nr_level) \
    X(ParamFloat, cfg_output_gain) \
    X(ParamFloat, cfg_pwr) \
    X(ParamInt, cfg_qsk_time) \
    X(ParamInt, cfg_rfgain) \
    X(ParamInt, cfg_rit) \
    X(ParamInt, cfg_speaker_off) \
    X(ParamInt, cfg_squelch) \
    X(ParamInt, cfg_swr_protect) \
    X(ParamInt, cfg_swrscan_quick) \
    X(ParamInt, cfg_transverter_0_from) \
    X(ParamFloat, cfg_transverter_0_pwr) \
    X(ParamInt, cfg_transverter_0_shift) \
    X(ParamInt, cfg_transverter_0_to) \
    X(ParamInt, cfg_transverter_1_from) \
    X(ParamFloat, cfg_transverter_1_pwr) \
    X(ParamInt, cfg_transverter_1_shift) \
    X(ParamInt, cfg_transverter_1_to) \
    X(ParamInt, cfg_tx_filter_high) \
    X(ParamInt, cfg_tx_filter_low) \
    X(ParamInt, cfg_tx_outside_bands) \
    X(ParamInt, cfg_volume) \
    X(ParamInt, cfg_vox_ag) \
    X(ParamInt, cfg_vox_delay) \
    X(ParamInt, cfg_vox_gain) \
    X(ParamInt, cfg_vox_on) \
    X(ParamInt, cfg_wr12) \
    X(ParamInt, cfg_xit) \

#define X(type, n) static fake_subj_t s_##n = {.name = #n}; type *n = (type *)&s_##n;
SETTINGS
#undef X

static fake_subj_t *subj(const void *p) {
    return (fake_subj_t *)p;
}

/* Its observers told, those subscribed by now (one added meanwhile waits for the next round, as in subject.cpp) */
static void notify(fake_subj_t *s) {
    int n = s->n_obs;

    for (int k = 0; k < n; k++) {
        s->obs[k].fn((Subject *)s, s->obs[k].ud);
    }
}

static void add_obs(fake_subj_t *s, observer_cb fn, void *ud) {
    if (s->n_obs == MAX_OBS) {
        fprintf(stderr, "fakes: %s: too many observers\n", s->name);
        exit(2);
    }
    s->obs[s->n_obs].fn = fn;
    s->obs[s->n_obs++].ud = ud;
}

static void set_value(fake_subj_t *s, int32_t v) {
    if (s->i != v) {
        s->i = v;
        notify(s);
    }
}

/* ComputedParameter::recompute, bound to a source */
static void recompute(Subject *src, void *ud) {
    fake_subj_t *s = ud;

    (void)src;
    if (s->updating) {
        return;
    }
    s->updating = true;
    set_value(s, s->compute());
    s->updating = false;
}

static int32_t fg_get(void) {
    return s_cfg_band_current_vfo.i == RADIO_VFO_A ? s_cfg_band_vfoa_freq.i : s_cfg_band_vfob_freq.i;
}

static int32_t bg_get(void) {
    return s_cfg_band_current_vfo.i == RADIO_VFO_A ? s_cfg_band_vfob_freq.i : s_cfg_band_vfoa_freq.i;
}

static void fg_set(int32_t f) {
    set_value(s_cfg_band_current_vfo.i == RADIO_VFO_A ? &s_cfg_band_vfoa_freq : &s_cfg_band_vfob_freq, f);
}

void fake_cfg_init(void) {
    s_cfg_band_vfoa_freq.i    = 7074000;
    s_cfg_band_vfob_freq.i    = 14074000;
    s_cfg_band_current_vfo.i  = RADIO_VFO_A;
    s_cfg_band_vfoa_mode.i    = radio_mode_usb;
    s_cfg_band_vfob_mode.i    = radio_mode_usb;
    s_cfg_pwr.f               = 5.0f;
    s_cfg_vox_on.i            = 1;
    s_cfg_key_train.i         = 0;
    s_cfg_wr12.i              = 0;
    s_cfg_tx_outside_bands.i  = 0;
    s_cfg_key_tone.i          = 700;
    s_cfg_qsk_time.i          = 100;
    s_cfg_key_speed.i         = 20;
    /* The dial and the other VFO, bound to the VFOs and the VFO in use before anyone else (settings_manager.cpp's
     * init_load, before the GUI's screen and radio.c) */
    s_cfg_fg_freq.compute = fg_get;
    s_cfg_fg_freq.reverse = fg_set;
    s_cfg_bg_freq.compute = bg_get;
    add_obs(&s_cfg_band_vfoa_freq, recompute, &s_cfg_fg_freq);
    add_obs(&s_cfg_band_vfob_freq, recompute, &s_cfg_fg_freq);
    add_obs(&s_cfg_band_current_vfo, recompute, &s_cfg_fg_freq);
    add_obs(&s_cfg_band_vfoa_freq, recompute, &s_cfg_bg_freq);
    add_obs(&s_cfg_band_vfob_freq, recompute, &s_cfg_bg_freq);
    add_obs(&s_cfg_band_current_vfo, recompute, &s_cfg_bg_freq);
    s_cfg_fg_freq.i = fg_get();
    s_cfg_bg_freq.i = bg_get();
    s_cfg_cur_mode.i = radio_mode_usb;
}

void set_i(ParamInt *p, int32_t v) {
    set_value(subj(p), v);
}

void set_quiet(ParamInt *p, int32_t v) {
    subj(p)->i = v;
}

void notify_now(ParamInt *p) {
    notify(subj(p));
}

void set_f(ParamFloat *p, float v) {
    fake_subj_t *s = subj(p);

    if (s->f != v) {
        s->f = v;
        notify(s);
    }
}

/* ComputedParameter::set: the reverse onto the sources, whose rounds pass this one by (updating), then its own */
static void computed_set(fake_subj_t *s, int32_t v) {
    if (s->updating || v == s->i) {
        return;
    }
    s->updating = true;
    if (s->reverse) {
        s->reverse(v);
    }
    set_value(s, s->compute ? s->compute() : v);
    s->updating = false;
}

void knob(int32_t freq) {
    computed_set(&s_cfg_fg_freq, freq);
}

int32_t param_i_get(const ParamInt *p) {
    return subj(p)->i;
}

void param_i_set(ParamInt *p, int32_t v) {
    set_value(subj(p), v);
}

float param_f_get(const ParamFloat *p) {
    return subj(p)->f;
}

int32_t cparam_i_get(const ComputedParamInt *p) {
    return subj(p)->i;
}

void cparam_i_set(ComputedParamInt *p, int32_t value) {
    computed_set(subj(p), value);
}

int32_t subject_i_get(SubjectInt *s) {
    return subj(s)->i;
}

float subject_f_get(SubjectFloat *s) {
    return subj(s)->f;
}

Observer *subject_subscribe(Subject *s, observer_cb fn, void *user_data) {
    add_obs(subj(s), fn, user_data);
    return (Observer *)s;
}

Observer *subject_subscribe_and_notify(Subject *s, observer_cb fn, void *user_data) {
    add_obs(subj(s), fn, user_data);
    fn(s, user_data);
    return (Observer *)s;
}

/* settings_manager.h: HF 0.1-55 MHz and the transverters with a shift transmit; the X6200 receives FM broadcast and
 * the airband, and 54 MHz-1 GHz with the WR12 on. A transverter wins where they overlap */
static int32_t shift_for(int32_t f) {
    if (s_cfg_transverter_0_shift.i && f >= s_cfg_transverter_0_from.i && f <= s_cfg_transverter_0_to.i) {
        return s_cfg_transverter_0_shift.i;
    }
    if (s_cfg_transverter_1_shift.i && f >= s_cfg_transverter_1_from.i && f <= s_cfg_transverter_1_to.i) {
        return s_cfg_transverter_1_shift.i;
    }
    return 0;
}

bool cfg_is_tx_freq(int32_t f) {
    return (f >= 100000 && f <= 55000000) || shift_for(f) != 0;
}

bool cfg_is_valid_hw_freq(int32_t f) {
    return cfg_is_tx_freq(f) || (f >= 87500000 && f <= 137000000) || (s_cfg_wr12.i && f >= 54000000 && f <= 1000000000);
}

bool cfg_wr12_freq(int32_t f) {
    return s_cfg_wr12.i && f >= 54000000 && f <= 1000000000 && shift_for(f) == 0;
}

int32_t cfg_transverter_shift_for(int32_t f) {
    return shift_for(f);
}

/* settings_manager_api.cpp's */
static const char *(*tx_block)(void);

void cfg_tx_block_set(const char *(*block)(void)) {
    tx_block = block;
}

const char *cfg_tx_block(void) {
    return tx_block ? tx_block() : NULL;
}

const char *cfg_tx_refusal(int32_t freq) {
    const char *block = cfg_tx_block();

    if (block) {
        return block;
    }
    switch (ham_tx_check(freq, cfg_is_tx_freq(freq), shift_for(freq) != 0, s_cfg_tx_outside_bands.i)) {
        case HAM_TX_RX_ONLY:
            return "receive only";
        case HAM_TX_OFF_BAND:
            return "outside the amateur bands (Settings > TX Cal)";
        default:
            return NULL;
    }
}

const char *cfg_tx_mhz(int32_t freq, char *buf, size_t size) {
    snprintf(buf, size, "%d.%03d MHz", (int)(freq / 1000000), (int)(freq % 1000000 / 1000));
    return buf;
}

void cfg_vfo_toggle(void) {
    set_value(&s_cfg_band_current_vfo, s_cfg_band_current_vfo.i == RADIO_VFO_A ? RADIO_VFO_B : RADIO_VFO_A);
}

float cfg_transverter_pwr_for(int32_t freq) {
    return shift_for(freq) ? s_cfg_transverter_0_pwr.f : 0.0f;
}

float cfg_transverter_pwr_cap(int32_t rx, int32_t tx) {
    float a = cfg_transverter_pwr_for(rx), b = cfg_transverter_pwr_for(tx);

    return a > 0.0f && b > 0.0f ? (a < b ? a : b) : (a > 0.0f ? a : b);
}

void cfg_transverter_name(int32_t freq, char *buf, size_t size) {
    (void)freq;
    snprintf(buf, size, "transverter");
}

pwr_xvtr_t cfg_pwr_xvtr_set(float watts, bool on_xvtr) {
    (void)watts;
    (void)on_xvtr;
    return PWR_XVTR_SAME;
}

void cfg_pwr_set(float watts) {
    set_f(cfg_pwr, watts);
}

void cfg_pwr_max_set(float watts) {
    (void)watts;
}

void cfg_hw_caps_set(uint32_t caps) {
    (void)caps;
}

uint32_t cfg_atu_get_network(void) {
    return 0;
}

Observer *cfg_atu_network_subscribe(observer_cb cb, void *user_data) {
    (void)cb;
    (void)user_data;
    return NULL;
}

int cfg_atu_save_network(uint32_t network) {
    (void)network;
    return 0;
}

void cfg_api_flush_all(void) {
}

void cfg_db_shutdown() {
}

/* The power-off's save of the old settings and its last good copy of params.db (params/db.c): none here */
void params_flush() {
}

bool database_keep(void) {
    return false;
}

/* The old settings (params.c): all 0, as nothing radio.c reads there matters here */
params_t params;

void params_lock() {
}

void params_unlock(bool *dirty) {
    (void)dirty;
}

void params_bool_set(params_bool_t *var, bool x) {
    var->x = x;
}

/* ---- The GUI thread's queue, the clock, the screen --------------------------------------------------------------- */

#define QUEUE 64

static struct {
    scheduler_fn_t fn;
    char           arg[64];
    bool           has_arg;
} queue[QUEUE];
static int n_queue;

static bool put(scheduler_fn_t fn, void *arg, size_t size, bool coalesced) {
    if (coalesced) {
        for (int k = 0; k < n_queue; k++) {
            if (queue[k].fn == fn) {
                return true;
            }
        }
    }
    if (n_queue == QUEUE || size > sizeof(queue[0].arg)) {
        return false;
    }
    queue[n_queue].fn = fn;
    queue[n_queue].has_arg = arg != NULL;
    if (arg) {
        memcpy(queue[n_queue].arg, arg, size);
    }
    n_queue++;
    return true;
}

bool scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size) {
    return put(fn, arg, arg_size, false);
}

bool scheduler_put_urgent(scheduler_fn_t fn, void *arg, size_t arg_size) {
    return put(fn, arg, arg_size, false);
}

bool scheduler_put_coalesced(scheduler_fn_t fn) {
    return put(fn, NULL, 0, true);
}

bool scheduler_on_gui_thread() {
    return true;
}

int gui_run(void) {
    int ran = 0;

    while (n_queue > 0 && ran < 1000) {
        char           arg[64];
        bool           has = queue[0].has_arg;
        scheduler_fn_t fn  = queue[0].fn;

        memcpy(arg, queue[0].arg, sizeof(arg));
        memmove(&queue[0], &queue[1], sizeof(queue[0]) * --n_queue);
        fn(has ? arg : NULL);
        ran++;
    }
    return ran;
}

static uint64_t clock_ms = 1000000;

uint64_t get_time() {
    return clock_ms;
}

void clock_add(uint64_t ms) {
    clock_ms += ms;
}

static char said_text[32][128];
static int  n_said;

static void say(const char *fmt, va_list ap) {
    if (n_said < 32) {
        vsnprintf(said_text[n_said++], sizeof(said_text[0]), fmt, ap);
        fprintf(stderr, "screen: %s\n", said_text[n_said - 1]);
    }
}

void msg_schedule_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    say(fmt, ap);
    va_end(ap);
}

void msg_schedule_long_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    say(fmt, ap);
    va_end(ap);
}

void msg_update_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    say(fmt, ap);
    va_end(ap);
}

bool said(const char *text) {
    for (int k = 0; k < n_said; k++) {
        if (strcmp(said_text[k], text) == 0) {
            return true;
        }
    }
    return false;
}

void said_clear(void) {
    n_said = 0;
}

void _lv_log_add(lv_log_level_t level, const char *file, int line, const char *func, const char *format, ...) {
    va_list ap;

    (void)level;
    (void)file;
    (void)line;
    (void)func;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void start_trace(const char *fmt, ...) {
    (void)fmt;
}

/* ---- The base: what radio.c sends it, on its way to hw_stub.c ---------------------------------------------------- */

base_t base = {.vfo = {-1, -1}, .pwr = -1.0f, .open_tx = 1};

/* The stand-in WR12's state (below): what radio.c asked, whether it is out of its bypass */
want_t      want;
static bool out;

bool __real_hw_vfo_set_freq(radio_vfo_t vfo, int32_t hz);
bool __real_hw_set_i(hw_param_t p, int32_t v);
bool __real_hw_set_f(hw_param_t p, float v);

/* Where the GUI refuses TX on the dial of the VFO in use (no split here), the WR12 aside: cfg_tx_refusal's bands */
static bool dial_refused(void) {
    int32_t dial = s_cfg_band_current_vfo.i == RADIO_VFO_A ? s_cfg_band_vfoa_freq.i : s_cfg_band_vfob_freq.i;

    return ham_tx_check(dial, cfg_is_tx_freq(dial), shift_for(dial) != 0, s_cfg_tx_outside_bands.i) != HAM_TX_OK;
}

/* After every write to the base and every wr12_want: the VFO in use on the WR12's IF, or the WR12 out of its bypass or
 * asked out of it, and not held, or keyed, or the base's own TX outside its band table open; the dial refused, and
 * the base open at its power */
static void base_look(const char *what) {
    bool        held = base.pwr == 0.0f && !base.vox && base.train && !base.ptt && !base.modem && !base.swrscan &&
                       base.open_tx == 0;
    const char *why  = base.vfo[base.cur] == WR12_IF_HZ ? "the VFO in use on the IF"
                       : out                         ? "the WR12 out of its bypass"
                       : want.rx                     ? "the WR12 asked out of its bypass"
                                                     : NULL;

    if (why && !held && !base.violations++) {
        snprintf(base.first, sizeof(base.first),
                 "%s: %s, VFO %c, PWR %.1f W, VOX %d, Train %d, PTT %d, modem %d, open TX %d", what, why,
                 "AB"[base.cur], base.pwr, base.vox, base.train, base.ptt, base.modem, base.open_tx);
    }
    if (!why && dial_refused() && base.open_tx != 0 && base.pwr != 0.0f && !base.violations++) {
        snprintf(base.first, sizeof(base.first), "%s: the dial where TX is refused, VFO %c, PWR %.1f W, open TX %d",
                 what, "AB"[base.cur], base.pwr, base.open_tx);
    }
}

bool __wrap_hw_vfo_set_freq(radio_vfo_t vfo, int32_t hz) {
    if (base.vfo[vfo] != hz) {
        base.vfo_writes++;
        base.vfo_writes_keyed += base.ptt;
        base.pwr_at_keyed_vfo = base.ptt && (int)vfo == base.cur ? base.pwr : base.pwr_at_keyed_vfo;
    }
    if ((base.ptt || base.modem) && (int)vfo == base.cur && base.n_keyed < 512) {
        base.keyed_hz[base.n_keyed++] = hz;
    }
    base.vfo[vfo] = hz;
    base.open_tx_at_vfo = base.open_tx;
    base_look("a VFO");
    return __real_hw_vfo_set_freq(vfo, hz);
}

bool __wrap_hw_set_i(hw_param_t p, int32_t v) {
    switch (p) {
        case HW_VFO_CUR:    base.cur = v; base.pwr_at_cur = base.pwr; base.open_tx_at_cur = base.open_tx; break;
        case HW_VOX:        base.vox = v; break;
        case HW_KEY_TRAIN:  base.train = v; break;
        case HW_IQ_SWAP:    base.iq = v; break;
        case HW_RIT:        base.rit = v; break;
        case HW_SPLIT:      base.split = v; break;
        case HW_PTT:
            base.open_tx_at_key = v ? base.open_tx : base.open_tx_at_key;
            base.pwr_at_ptt = v ? base.pwr : base.pwr_at_ptt;
            base.ptt = v;
            break;
        case HW_MODEM:      base.open_tx_at_key = v ? base.open_tx : base.open_tx_at_key; base.modem = v; break;
        case HW_SWRSCAN:    base.open_tx_at_key = v ? base.open_tx : base.open_tx_at_key; base.swrscan = v; break;
        case HW_OPEN_TX:
            base.open_tx_writes++;
            if (base.open_tx_fails > 0) {
                base.open_tx_fails--;
                return false;   /* as hw_x6200.c when the block is not as it must be: not written */
            }
            base.open_tx = v;
            break;
        default:            break;
    }
    base_look("a setting");
    return __real_hw_set_i(p, v);
}

void __real_hw_pin_set(hw_pin_t pin, bool on);

void __wrap_hw_pin_set(hw_pin_t pin, bool on) {
    if (pin == HW_PIN_MORSE_KEY) {
        if (on) {
            base.open_tx_at_key = base.open_tx;
        }
        atomic_store(&base.key, on);
    }
    __real_hw_pin_set(pin, on);
}

bool __wrap_hw_set_f(hw_param_t p, float v) {
    if (p == HW_TXPWR_F) {
        base.pwr = v;
    }
    base_look("the power");
    return __real_hw_set_f(p, v);
}

/* The radio's own thread (radio_thread: the base's packets) is not started: no packets, nothing keys by itself */
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    (void)attr;
    (void)fn;
    (void)arg;
    *t = pthread_self();
    return 0;
}

int __wrap_pthread_detach(pthread_t t) {
    (void)t;
    return 0;
}

/* ---- The stand-in WR12 driver ------------------------------------------------------------------------------------ */

int fake_errors;

static bool (*wr12_notify_fn)(void);
static bool   ready;

void wr12_init(bool (*notify)(void), bool (*keyed)(void)) {
    (void)keyed;
    wr12_notify_fn = notify;
}

void wr12_want(bool on, bool rx, int32_t hz) {
    want.on = on;
    want.rx = rx;
    if (rx) {
        want.hz = hz;
    }
    want.n++;
    base_look("wr12_want");
}

bool wr12_ready(void) {
    return ready;
}

const char *wr12_tx_block(void) {
    return want.rx ? "WR12 receiving" : out ? "WR12 not in bypass" : NULL;
}

bool wr12_note(char *buf, size_t size) {
    (void)buf;
    (void)size;
    return false;
}

void wr12_found(void) {
    ready = true;
    wr12_notify_fn();
}

void wr12_went_out(void) {
    if (!want.on || !want.rx) {
        printf("FAIL the WR12 cannot go out of its bypass: radio.c does not ask for it\n");
        fake_errors++;
        return;
    }
    out = true;
    base_look("the WR12 out");
    wr12_notify_fn();
}

void wr12_back_in(void) {
    out = false;
    wr12_notify_fn();
}

void wr12_lost(void) {
    ready = false;
    wr12_notify_fn();
}

void wr12_gone(void) {
    out = false;
    wr12_notify_fn();
}

void fake_start_out(void) {
    out = true;
}

/* ---- The rest, as an idle radio: nothing on the air, nothing open --------------------------------------------- */

int16_t backlight_level() {
    return 50;
}

void bat_est_feed(const bat_sample_t *s) {
    shown.fed = s->cap;
}

bool bat_est_load(const char *path) {
    (void)path;
    return false;
}

int bat_est_minutes(int to_cap, bool *charging, bool *approx) {
    (void)to_cap;
    *charging = supply.vext > 0;
    *approx   = false;
    return MODEL_MIN;
}

bool bat_est_replay(const char *path) {
    (void)path;
    return false;
}

bool bat_est_save(const char *path) {
    (void)path;
    return false;
}

/* The base's supply in the packets (hw_flow_read wrapped): only the test's turns of radio_tick read them */
supply_t supply;
shown_t  shown = {.cap = -1, .fed = -1, .est = -1};

bool __wrap_hw_flow_read(hw_pack_t *p) {
    memset(p, 0, sizeof(*p));
    p->tx        = supply.tx;
    p->vbat      = supply.vbat;
    p->vext      = supply.vext;
    p->vext_flag = supply.vext > 0;
    p->charging  = supply.charging;
    p->batcap    = supply.batcap;
    return true;
}

/* The base's data taken as flowing (#152's rule, which tools/base_guard tests): the radio thread is not started here */
bool __wrap_base_no_data(uint64_t now_ms, uint64_t pack_ms, bool seen) {
    (void)now_ms;
    (void)pack_ms;
    (void)seen;
    return false;
}

/* The base log taken as free: its one line a second is tools/base_guard's, and the clock here is the test's */
bool __wrap_base_guard_log_free(void) {
    return true;
}

bool btaudio_speaker_muted(void) {
    return false;
}

void clock_update_estimate(int minutes, bool approx) {
    (void)approx;
    shown.est = minutes;
}

void clock_update_no_battery(bool none) {
    (void)none;
}

void clock_update_power(float ext, float bat, uint8_t cap, bool charge) {
    (void)ext;
    (void)bat;
    (void)charge;
    shown.cap = cap;
}

void cw_encoder_stop() {
}

static bool trainer;

void fake_trainer(bool open) {
    trainer = open;
}

bool cw_trainer_on(void) {
    return trainer;
}

void dialog_msg_voice_ptt_pressed(void) {
}

void dialog_swrscan_give_up(const char *why) {
    (void)why;
}

bool swrscan_stop_runs;

void dialog_swrscan_stop(int32_t freq) {
    (void)freq;
    if (swrscan_stop_runs) {
        swrscan_stop_runs = false;
        radio_stop_swrscan();
        radio_set_tx_freq(0);
    }
}

void dialog_swrscan_update(float vswr) {
    (void)vswr;
}

void dsp_psd(const float *db, uint16_t n, uint32_t span_hz, bool tx, int16_t smeter_dbm) {
    (void)db;
    (void)n;
    (void)span_hz;
    (void)tx;
    (void)smeter_dbm;
}

void dsp_reset() {
}

void dsp_samples(cfloat *buf_samples, uint16_t size, bool tx, uint32_t base_freq, bool vary_freq, uint8_t fft_dec) {
    (void)buf_samples;
    (void)size;
    (void)tx;
    (void)base_freq;
    (void)vary_freq;
    (void)fft_dec;
}

void hkey_put(uint32_t key) {
    (void)key;
}

bool key_limit_over(uint32_t now_ms, uint32_t down_ms, uint32_t limit_ms) {
    (void)now_ms;
    (void)down_ms;
    (void)limit_ms;
    return false;
}

void keypad_power_set(bool down) {
    (void)down;
}

int32_t limit(int32_t x, int32_t min, int32_t max) {
    return x < min ? min : x > max ? max : x;
}

/* The supply the S-meter takes its attenuator figure for (#125), as radio.c told it last */
bool meter_battery;

void meter_set_battery(bool battery) {
    meter_battery = battery;
}

void paddle_keyer_stop(void) {
}

void screen_rec_shutdown() {
}

void settings_window_poweroff(void) {
}

bool swr_guard_check(swr_guard_t *g, float pwr, float swr, float lim, uint64_t now_ms) {
    (void)g;
    (void)pwr;
    (void)swr;
    (void)lim;
    (void)now_ms;
    return false;
}

float tune_swr_limit(const tune_guard_t *t, float pwr_w, float setting, uint64_t now_ms) {
    (void)t;
    (void)pwr_w;
    (void)now_ms;
    return setting;
}

void tx_info_update(float pwr, float swr, float alc) {
    (void)pwr;
    (void)swr;
    (void)alc;
}

void tx_info_clear(void) {
}

void freq_strip_set_wr12(bool in_use) {
    (void)in_use;
}

wifi_status_t wifi_get_status() {
    return (wifi_status_t)0;
}
