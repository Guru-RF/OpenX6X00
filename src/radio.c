/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

/*********************
 *      INCLUDES
 *********************/
#include "radio.h"
#include "board.h"

#include "hw/hw.h"

#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>


#include "cfg/cfg_api.h"
#include "cfg/db.h"
#include "util.h"
#include "dsp.h"
#include "screen_rec.h"
#include "params/params.h"
#include "hkey.h"
#include "tx_info.h"
#include "info.h"
#include "dialog_swrscan.h"
#include "tx_player.h"
#include "cw.h"
#include "btaudio.h"
#include "pubsub_ids.h"
#include "scheduler.h"
#include "front_keys.h"
#include "backlight.h"
#include "swr_guard.h"
#include "paddle_keyer.h"
#include "cw_encoder.h"
#include "dialog_msg_voice.h"
#include "msg.h"
#include "base_guard.h"
#include "audio.h"
#include "bat_est.h"
#include "clock.h"
#include "wifi.h"

/*********************
 *      DEFINES
 *********************/

#define FLOW_RESTART_TIMEOUT 300
#define IDLE_TIMEOUT        (3 * 1000)
#define RESYNC_RETRY_MS     200     /* the register block again, after one that didn't get through */
#define MORSE_HANG_MS       3000    /* the base holds TX for the QSK time after the GUI's CW key */

#define FILTER_2_OFFSET 60
#define FILTER_2_OFFSET_IN 20
#define FILTER_2_OFFSET_OUT (FILTER_2_OFFSET - FILTER_2_OFFSET_IN)

/* Battery voltage in 0.1 V units; LOW + HYSTERESIS = HIGH (upstream 21c2b93) */
#define LOW_BATTERY_VBAT_LOW     60
#define LOW_BATTERY_HYSTERESIS   2
#define LOW_BATTERY_VBAT_HIGH    (LOW_BATTERY_VBAT_LOW + LOW_BATTERY_HYSTERESIS)

/**********************
 *      MACROS
 **********************/

#define WITH_RADIO_LOCK(fn) radio_lock(); fn; radio_unlock();

#define CHANGE_PARAM(new_val, val, dirty, hw_param) \
    if (new_val != val) { \
        params_lock(); \
        val = new_val; \
        params_unlock(&dirty); \
        radio_lock(); \
        hw_set_i(hw_param, val); \
        radio_unlock(); \
    }

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void radio_lock();
static void radio_unlock();

static void on_vfo_freq_change(Subject *subj, void *user_data);
static void on_vfo_mode_change(Subject *subj, void *user_data);
static void on_vfo_agc_change(Subject *subj, void *user_data);
static void on_cw_agc_cond_change(Subject *subj, void *user_data);
static void on_vfo_att_change(Subject *subj, void *user_data);
static void on_vfo_pre_change(Subject *subj, void *user_data);
static void on_low_filter_change(Subject *subj, void *user_data);
static void on_high_filter_change(Subject *subj, void *user_data);

static void on_if_shift_change(Subject *subj, void *user_data);
static void on_band_change(Subject *subj, void *user_data);
static void update_agc_time(Subject *subj, void *user_data);
static void on_atu_network_change(Subject *subj, void *user_data);
static void on_change_comp_ratio(Subject *subj, void *user_data);
static void on_fw_zoom_change(Subject *subj, void *user_data);

static void on_change_i(Subject *subj, void *user_data);
static void on_change_f(Subject *subj, void *user_data);
static void on_pwr_change(Subject *subj, void *user_data);
static void on_volume_change(Subject *subj, void *user_data);

static void recover_processing_audio_inputs();
static bool radio_tick();
static void * radio_thread(void *arg);

/* The tuner found its network, on the LVGL thread (see the flow handler) */
static void atu_done_cb(void *arg) {
    cfg_atu_save_network(*(uint32_t *)arg);
    param_i_set(cfg_atu_enabled, true);
}

/* A hand mic key, on the LVGL thread (see the flow handler) */
static void hkey_sched_cb(void *arg) {
    hkey_put(*(uint32_t *)arg);
}

/**********************
 *  STATIC VARIABLES
 **********************/

static radio_rx_tx_change_t notify_rx_tx;
static void(*low_power_cb)(bool) = NULL;
static bool low_power = false;

static pthread_mutex_t  control_mux;

static hw_pack_t        pack_buf;
static hw_pack_t       *pack = &pack_buf;
static radio_base_ver_t base_ver;      /* the base's firmware, read at init */
static int8_t           audio_in_lvl;

/* Written by the radio thread (RX/TX and the tuner's steps, from what the base reports) and by
 * the LVGL thread (the tuner and the SWR scan started or stopped, power off): atomic, and
 * every change goes from the state the writer saw, so a change by the other thread in between
 * is not overwritten (one of the two would be lost otherwise) */
static _Atomic radio_state_t state = RADIO_RX;

static bool state_move(radio_state_t from, radio_state_t to) {
    return atomic_compare_exchange_strong(&state, &from, to);
}
static uint64_t         now_time;
static uint64_t         prev_time;
static uint64_t         idle_time;
static uint64_t         start_time;     /* radio_init() */
static uint64_t         pack_time;      /* last packet from the base */
static uint8_t          pack_vbat, pack_vext;   /* its voltages, 0.1 V */
static volatile int8_t  bat_pct = -1;   /* the battery %, -1 on external power or not measured yet */
static bool             mute = false;
static bool             tci_mute = false;   /* a TCI client transmits: the speaker and the monitor off */
static bool             digi_cap;       /* a digital app is open: never above its cap */

static tx_watch_t       tx_watch;
static bool             resync_due;     /* resend the whole register block (base_guard.h) */
static uint64_t         resync_time;
static volatile bool    morse_on;
static volatile uint32_t morse_ms;      /* the GUI's CW key last changed */

static void power_key_cb(void *arg) {
    keypad_power_set(*(bool *)arg);
}

static void set_txpwr(float w) {
    hw_set_f(HW_TXPWR_F, digi_cap && w > TX_PLAYER_MAX_PWR_W ? TX_PLAYER_MAX_PWR_W : w);
}

static uint32_t min_tx;
static uint32_t max_tx;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void radio_bb_reset() {
    hw_bb_reset();
}

void radio_init() {
    if (!hw_init())
        return;

    /* Back off (0.1 .. 25.6 s) and, once, after about 6 s without an answer,
     * pulse the baseband reset pin instead of waiting forever (upstream 37f3894) */
    uint8_t cnt = 0;

    while (!hw_control_init()) {
        usleep(100000 << cnt);
        if (cnt < 8) cnt++;
        if (cnt == 6) {
            fprintf(stderr, "Baseband not answering, reset\n");
            radio_bb_reset();
            usleep(1000000);
        }
    }
}

void radio_start() {

    hw_start();
    base_ver = hw_base_ver();

    subject_subscribe_and_notify((Subject*)cfg_band_vfoa_freq, on_vfo_freq_change, (void*)RADIO_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg_band_vfob_freq, on_vfo_freq_change, (void*)RADIO_VFO_B);

    subject_subscribe_and_notify((Subject*)cfg_band_vfoa_mode, on_vfo_mode_change, (void*)RADIO_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg_band_vfob_mode, on_vfo_mode_change, (void*)RADIO_VFO_B);

    subject_subscribe_and_notify((Subject*)cfg_band_vfoa_agc, on_vfo_agc_change, (void*)RADIO_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg_band_vfob_agc, on_vfo_agc_change, (void*)RADIO_VFO_B);
    subject_subscribe((Subject*)cfg_band_vfoa_mode, on_cw_agc_cond_change, NULL);
    subject_subscribe((Subject*)cfg_band_vfob_mode, on_cw_agc_cond_change, NULL);
    subject_subscribe((Subject*)cfg_cw_decoder, on_cw_agc_cond_change, NULL);

    subject_subscribe_and_notify((Subject*)cfg_band_vfoa_att, on_vfo_att_change, (void*)RADIO_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg_band_vfob_att, on_vfo_att_change, (void*)RADIO_VFO_B);

    subject_subscribe_and_notify((Subject*)cfg_band_vfoa_pre, on_vfo_pre_change, (void*)RADIO_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg_band_vfob_pre, on_vfo_pre_change, (void*)RADIO_VFO_B);

    subject_subscribe_and_notify((Subject*)cfg_band_current_vfo, on_change_i, (void*)HW_VFO_CUR);
    subject_subscribe_and_notify((Subject*)cfg_band_split, on_change_i, (void*)HW_SPLIT);
    subject_subscribe_and_notify((Subject*)cfg_rfgain, on_change_i, (void*)HW_RFG);

    subject_subscribe_and_notify((Subject*)cfg_band_if_shift, on_if_shift_change, NULL);

    subject_subscribe_and_notify((Subject*)cfg_band_tx_i_offset, on_change_i, (void*)HW_TX_I_OFFSET);
    subject_subscribe_and_notify((Subject*)cfg_band_tx_q_offset, on_change_i, (void*)HW_TX_Q_OFFSET);

    subject_subscribe((Subject*)cfg_band_id, on_band_change, NULL);

    subject_subscribe((Subject*)cfg_cur_agc, update_agc_time, NULL);
    subject_subscribe_and_notify((Subject*)cfg_cur_mode, update_agc_time, NULL);

    subject_subscribe((Subject*)cfg_cur_filter_low, on_low_filter_change, NULL);
    subject_subscribe_and_notify((Subject*)cfg_cur_mode, on_low_filter_change, NULL);
    subject_subscribe((Subject*)cfg_cur_filter_high, on_high_filter_change, NULL);
    subject_subscribe_and_notify((Subject*)cfg_cur_mode, on_high_filter_change, NULL);

    subject_subscribe_and_notify((Subject*)cfg_volume, on_volume_change, NULL);
    subject_subscribe_and_notify((Subject*)cfg_squelch, on_change_i, (void*)HW_SQL);
    subject_subscribe_and_notify((Subject*)cfg_pwr, on_pwr_change, NULL);
    subject_subscribe_and_notify((Subject*)cfg_output_gain, on_change_f, (void*)HW_ADC_DAC_GAIN_F);
    subject_subscribe_and_notify((Subject*)cfg_band_dac_offset, on_change_f, (void*)HW_DAC_GAIN_F);
    subject_subscribe_and_notify((Subject*)cfg_atu_enabled, on_change_i, (void*)HW_ATU_ENABLED);
    cfg_atu_network_subscribe(on_atu_network_change, NULL);
    on_atu_network_change(NULL, NULL);

    /* Compressor */
    subject_subscribe_and_notify((Subject*)cfg_comp, on_change_comp_ratio, NULL);
    subject_subscribe_and_notify((Subject*)cfg_comp_threshold_offset, on_change_f, (void*)HW_COMP_THRESHOLD_F);
    subject_subscribe_and_notify((Subject*)cfg_comp_makeup_offset, on_change_f, (void*)HW_COMP_MAKEUP_F);

    /* VOX */
    subject_subscribe_and_notify((Subject*)cfg_vox_on, on_change_i, (void*)HW_VOX);
    subject_subscribe_and_notify((Subject*)cfg_vox_gain, on_change_i, (void*)HW_VOX_GAIN);
    subject_subscribe_and_notify((Subject*)cfg_vox_ag, on_change_i, (void*)HW_VOX_AG);
    subject_subscribe_and_notify((Subject*)cfg_vox_delay, on_change_i, (void*)HW_VOX_DELAY);

    subject_subscribe_and_notify((Subject*)cfg_mic, on_change_i, (void*)HW_MIC);
    subject_subscribe_and_notify((Subject*)cfg_hmic, on_change_i, (void*)HW_HMIC);
    subject_subscribe_and_notify((Subject*)cfg_imic, on_change_i, (void*)HW_IMIC);
    subject_subscribe_and_notify((Subject*)cfg_moni, on_change_i, (void*)HW_MONI);

    subject_subscribe_and_notify((Subject*)cfg_rit, on_change_i, (void*)HW_RIT);
    subject_subscribe_and_notify((Subject*)cfg_xit, on_change_i, (void*)HW_XIT);
    subject_subscribe_and_notify((Subject*)cfg_fm_emphasis, on_change_i, (void*)HW_FM_EMP);

    subject_subscribe_and_notify((Subject*)cfg_tx_filter_low, on_change_i, (void*)HW_TX_FILTER_LOW);
    subject_subscribe_and_notify((Subject*)cfg_tx_filter_high, on_change_i, (void*)HW_TX_FILTER_HIGH);

    subject_subscribe_and_notify((Subject*)cfg_cessb_on, on_change_i, (void*)HW_CESSB);
    subject_subscribe_and_notify((Subject*)cfg_cessb_power_up, on_change_f, (void*)HW_CESSB_POWER_UP_F);

    subject_subscribe_and_notify((Subject*)cfg_key_tone, on_change_i, (void*)HW_KEY_TONE);
    subject_subscribe_and_notify((Subject*)cfg_key_speed, on_change_i, (void*)HW_KEY_SPEED);
    subject_subscribe_and_notify((Subject*)cfg_key_mode, on_change_i, (void*)HW_KEY_MODE);
    subject_subscribe_and_notify((Subject*)cfg_iambic_mode, on_change_i, (void*)HW_IAMBIC_MODE);
    subject_subscribe_and_notify((Subject*)cfg_key_vol, on_change_i, (void*)HW_KEY_VOL);
    subject_subscribe_and_notify((Subject*)cfg_key_train, on_change_i, (void*)HW_KEY_TRAIN);
    subject_subscribe_and_notify((Subject*)cfg_qsk_time, on_change_i, (void*)HW_QSK_TIME);
    subject_subscribe_and_notify((Subject*)cfg_key_ratio, on_change_f, (void*)HW_KEY_RATIO_F);
    subject_subscribe_and_notify((Subject*)cfg_cw_peak_on, on_change_i, (void*)HW_CW_PEAK);
    subject_subscribe_and_notify((Subject*)cfg_cw_peak_q, on_change_i, (void*)HW_CW_PEAK_Q);

    subject_subscribe_and_notify((Subject*)cfg_agc_hang, on_change_i, (void*)HW_AGC_HANG);
    subject_subscribe_and_notify((Subject*)cfg_agc_knee, on_change_i, (void*)HW_AGC_KNEE);
    subject_subscribe_and_notify((Subject*)cfg_agc_slope, on_change_i, (void*)HW_AGC_SLOPE);

    subject_subscribe_and_notify((Subject*)cfg_dnf, on_change_i, (void*)HW_DNF);
    subject_subscribe_and_notify((Subject*)cfg_dnf_center, on_change_i, (void*)HW_DNF_CENTER);
    subject_subscribe_and_notify((Subject*)cfg_dnf_width, on_change_i, (void*)HW_DNF_WIDTH);
    subject_subscribe_and_notify((Subject*)cfg_dnf_auto, on_change_i, (void*)HW_DNF_AUTO);
    subject_subscribe_and_notify((Subject*)cfg_nb, on_change_i, (void*)HW_NB);
    subject_subscribe_and_notify((Subject*)cfg_nb_level, on_change_i, (void*)HW_NB_LEVEL);
    subject_subscribe_and_notify((Subject*)cfg_nb_width, on_change_i, (void*)HW_NB_WIDTH);
    subject_subscribe_and_notify((Subject*)cfg_nr, on_change_i, (void*)HW_NR);
    subject_subscribe_and_notify((Subject*)cfg_nr_level, on_change_i, (void*)HW_NR_LEVEL);

    if (hw_caps() & HW_CAP_ZOOM) {
        subject_subscribe_and_notify((Subject*)cfg_mode_zoom, on_fw_zoom_change, NULL);
    }

    hw_set_i(HW_CHARGER, params.charger.x == RADIO_CHARGER_ON);
    hw_set_i(HW_BIAS_DRIVE, params.bias_drive);
    hw_set_i(HW_BIAS_FINAL, params.bias_final);
    hw_set_i(HW_SPMODE, params.spmode.x);
    hw_set_i(HW_LINE_IN, params.line_in);
    hw_set_i(HW_LINE_OUT, params.line_out);

    prev_time = get_time();
    idle_time = prev_time;
    start_time = prev_time;
    pack_time = prev_time;

    pthread_mutex_init(&control_mux, NULL);

    pthread_t thread;

    pthread_create(&thread, NULL, radio_thread, NULL);
    pthread_detach(thread);
}

void radio_set_rx_tx_notify_fn(radio_rx_tx_change_t cb) {
    notify_rx_tx = cb;
}

/* On the LVGL thread: low_power_cb creates and deletes timers */
static void low_power_ui_cb(void *arg) {
    if (low_power_cb) {
        low_power_cb(*(bool *)arg);
    }
}

void radio_set_low_power_cb(void (*cb)(bool)) {
    low_power_cb = cb;
}

radio_state_t radio_get_state() {
    return state;
}

void radio_set_freq(int32_t freq) {
    if (!radio_check_freq(freq)) {
        LV_LOG_ERROR("Freq %i incorrect", freq);
        return;
    }
    radio_vfo_t vfo = param_i_get(cfg_band_current_vfo);
    int32_t shift = cfg_transverter_shift_for(freq);
    WITH_RADIO_LOCK(hw_vfo_set_freq(vfo, freq - shift));
}

bool radio_check_freq(int32_t freq) {
    return cfg_is_valid_hw_freq(freq);
}

uint16_t radio_change_vol(int16_t df) {
    int32_t vol = param_i_get(cfg_volume);
    if (df == 0) {
        return vol;
    }

    mute = false;

    uint16_t new_val = limit(vol + df, 0, 55);

    if (new_val != vol) {
        param_i_set(cfg_volume, new_val);
    };

    return new_val;
}

void radio_change_mute() {
    mute = !mute;
    radio_vol_refresh();
}

/* Speaker AF: 0 when muted, when btaudio silences it (headphones connected) or while a TCI
 * client transmits */
void radio_vol_refresh() {
    WITH_RADIO_LOCK(hw_set_i(HW_RXVOL, (mute || tci_mute || btaudio_speaker_muted()) ? 0 : param_i_get(cfg_volume)));
}

/* A TCI client keyed the radio: the operator is at the other end, so the speaker and the TX
 * monitor stay quiet here until it unkeys; then both come back as they were */
void radio_set_tci_mute(bool on) {
    if (tci_mute == on) return;
    tci_mute = on;
    radio_vol_refresh();
    WITH_RADIO_LOCK(hw_set_i(HW_MONI, on ? 0 : param_i_get(cfg_moni)));
}

static void on_volume_change(Subject *subj, void *user_data) {
    radio_vol_refresh();
}

bool radio_change_spmode(int16_t df) {
    if (df == 0) {
        return params.spmode.x;
    }

    params_bool_set(&params.spmode, df > 0);

    WITH_RADIO_LOCK(hw_set_i(HW_SPMODE, params.spmode.x));

    return params.spmode.x;
}

void radio_start_atu() {
    state_move(RADIO_RX, RADIO_ATU_START);
}

bool radio_start_swrscan() {
    if (!state_move(RADIO_RX, RADIO_SWRSCAN)) {
        return false;
    }

    cparam_i_set(cfg_cur_mode, radio_mode_am);
    radio_lock();
    /* 1 W is enough to read the SWR; less if PWR is set lower.
     * ponytail: fixed 1 W cap, make it a setting if the bridge reads poorly that low */
    hw_set_f(HW_TXPWR_F, LV_MIN(param_f_get(cfg_pwr), 1.0f));
    hw_set_i(HW_SWRSCAN, true);
    radio_unlock();

    return true;
}

void radio_stop_swrscan() {
    if (state_move(RADIO_SWRSCAN, RADIO_RX)) {
        radio_lock();
        hw_set_i(HW_SWRSCAN, false);
        set_txpwr(param_f_get(cfg_pwr));
        radio_unlock();
    }
}

void radio_set_pwr(float d) {
    WITH_RADIO_LOCK(set_txpwr(d));
}

void radio_set_digi_cap(bool on) {
    digi_cap = on;
    radio_set_pwr(param_f_get(cfg_pwr));
}

void radio_set_tx_filter(uint16_t from_hz, uint16_t to_hz) {
    WITH_RADIO_LOCK(hw_set_i(HW_TX_FILTER_LOW, from_hz); hw_set_i(HW_TX_FILTER_HIGH, to_hz));
}

void radio_set_rx_dsp_off(bool all_off) {
    bool nr = param_i_get(cfg_nr), nb = param_i_get(cfg_nb);
    bool dnf = param_i_get(cfg_dnf), dnf_auto = param_i_get(cfg_dnf_auto);

    WITH_RADIO_LOCK(hw_set_i(HW_NR, nr && !all_off);
                    hw_set_i(HW_NB, nb && !all_off);
                    hw_set_i(HW_DNF, dnf && !all_off);
                    hw_set_i(HW_DNF_AUTO, dnf_auto && !all_off));
}

radio_vfo_t radio_toggle_vfo() {
    cfg_vfo_toggle();
    return (radio_vfo_t)param_i_get(cfg_band_current_vfo);
}

void radio_poweroff() {
    screen_rec_shutdown();
    if (params.charger.x == RADIO_CHARGER_SHADOW) {
        WITH_RADIO_LOCK(hw_set_i(HW_CHARGER, true));
    }
    cfg_api_flush_all();
    cfg_db_shutdown();
    state = RADIO_POWEROFF;
}

void radio_set_charger(bool on) {
    WITH_RADIO_LOCK(hw_set_i(HW_CHARGER, on));
}

void radio_set_ptt(bool tx) {
    WITH_RADIO_LOCK(hw_set_i(HW_PTT, tx));
}

static int mic_override = -1;      /* the walkie screen's mic, cfg_mic stays as saved */

void radio_set_mic_override(int mic) {
    mic_override = mic;
    WITH_RADIO_LOCK(hw_set_i(HW_MIC, mic < 0 ? param_i_get(cfg_mic) : mic));
}

int radio_get_mic(void) {
    return mic_override < 0 ? param_i_get(cfg_mic) : mic_override;
}

static bool modem_on;

void radio_set_modem(bool tx) {
    modem_on = tx;
    WITH_RADIO_LOCK(hw_set_i(HW_MODEM, tx));
}

bool radio_get_modem(void) {
    return modem_on;
}

/* Under the lock: the record, PTT, modem, tune and split bits share one register, and the
 * library's read-modify-write of it from two threads lost a PTT or modem change */
static void play_mode_apply(audio_play_mode_t mode) {
    switch (mode) {
    case AUDIO_PLAY_OFF:
        hw_set_i(HW_RECORD, false);
        hw_set_i(HW_HMIC, param_i_get(cfg_hmic));
        hw_set_i(HW_IMIC, param_i_get(cfg_imic));
        break;

    case AUDIO_PLAY_ON:
        hw_set_i(HW_HMIC, 0);
        /* fall through */
    case AUDIO_PLAY_VOICE_REC:
        hw_set_i(HW_IMIC, 0);
        hw_set_i(HW_RECORD, true);
        break;

    default:
        break;
    }
}

void radio_set_play_mode(audio_play_mode_t mode) {
    WITH_RADIO_LOCK(play_mode_apply(mode));
}

radio_base_ver_t radio_base_ver(void) {
    return hw_base_ver();
}

uint32_t radio_caps(void) {
    return hw_caps();
}

const char *radio_base_ver_str(void) {
    return hw_base_ver_str();
}

void radio_wifi_power(bool on) {
    hw_pin_set(HW_PIN_WIFI, on);
}

void radio_backlight_power(bool on) {
    hw_pin_set(HW_PIN_LIGHT, on);
}

void radio_usb_to_cat(void) {
    hw_pin_set(HW_PIN_USB_CAT, true);
}

void radio_speaker_play(bool speaker) {
    radio_set_play_mode(speaker ? AUDIO_PLAY_ON : AUDIO_PLAY_OFF);
}

void radio_set_line_in(uint8_t d) {
    CHANGE_PARAM(d, params.line_in, params.dirty.line_in, HW_LINE_IN);
}

void radio_set_line_out(uint8_t d) {
    CHANGE_PARAM(d, params.line_out, params.dirty.line_out, HW_LINE_OUT);
}

void radio_set_qsk_time(uint16_t ms) {
    WITH_RADIO_LOCK(hw_set_i(HW_QSK_TIME, ms));
}

void radio_set_rec_moni(bool on) {
    int32_t moni = param_i_get(cfg_moni);

    if (tci_mute) return;       /* quiet while a TCI client transmits */
    WITH_RADIO_LOCK(hw_set_i(HW_MONI, on && moni < 30 ? 30 : moni));
}

void radio_set_morse_key(bool on) {
    morse_on = on;
    morse_ms = get_time();
    hw_pin_set(HW_PIN_MORSE_KEY, on);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

 static void radio_lock() {
    pthread_mutex_lock(&control_mux);
}

static void radio_unlock() {
    idle_time = get_time();
    pthread_mutex_unlock(&control_mux);
}

/* A setting straight to the base: user_data is the hw_param_t */
static void on_change_i(Subject *subj, void *user_data) {
    int32_t v = subject_i_get((SubjectInt*)subj);
    WITH_RADIO_LOCK(hw_set_i((hw_param_t)(intptr_t)user_data, v));
}

static void on_change_f(Subject *subj, void *user_data) {
    float v = subject_f_get((SubjectFloat*)subj);
    WITH_RADIO_LOCK(hw_set_f((hw_param_t)(intptr_t)user_data, v));
}

static void on_pwr_change(Subject *subj, void *user_data) {
    WITH_RADIO_LOCK(set_txpwr(subject_f_get((SubjectFloat*)subj)));
}

static void on_vfo_freq_change(Subject *subj, void *user_data) {
    radio_vfo_t vfo = (radio_vfo_t )user_data;
    int32_t new_val;
    if (vfo == RADIO_VFO_A) {
        new_val = param_i_get(cfg_band_vfoa_freq);
    } else {
        new_val = param_i_get(cfg_band_vfob_freq);
    }
    int32_t shift = cfg_transverter_shift_for(new_val);
    shift += param_i_get(cfg_band_if_shift);
    radio_lock();
    bool changed = hw_vfo_set_freq(vfo, new_val - shift);
    radio_unlock();
    if (changed) {
        LV_LOG_USER("Radio set vfo %i freq=%i (%i)", vfo, new_val, new_val - shift);
    }
}

static void on_vfo_mode_change(Subject *subj, void *user_data) {
    radio_vfo_t vfo = (radio_vfo_t )user_data;
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    radio_lock();
    bool changed = hw_vfo_set_mode(vfo, new_val);
    radio_unlock();
    if (changed) {
        LV_LOG_USER("Radio set vfo %i mode=%i", vfo, new_val);
    }
}

/* AGC AUTO in CW with the decoder on goes FAST: as AUTO recovers after a station stops, the rising
 * noise keys the decoder and prints Ts and Es between its overs (40 m, 2026-10-14). Only the
 * radio gets FAST: the setting stays AUTO, and an explicit OFF / SLOW / FAST is left alone. */
static void apply_vfo_agc(radio_vfo_t vfo) {
    int32_t agc  = subject_i_get((SubjectInt*)(vfo == RADIO_VFO_A ? cfg_band_vfoa_agc : cfg_band_vfob_agc));
    int32_t mode = subject_i_get((SubjectInt*)(vfo == RADIO_VFO_A ? cfg_band_vfoa_mode : cfg_band_vfob_mode));

    if (agc == radio_agc_auto && (mode == radio_mode_cw || mode == radio_mode_cwr) && subject_i_get((SubjectInt*)cfg_cw_decoder)) {
        agc = radio_agc_fast;
    }
    WITH_RADIO_LOCK(hw_vfo_set_agc(vfo, agc));
    LV_LOG_USER("Radio set vfo %i agc=%i", vfo, agc);
}

static void on_vfo_agc_change(Subject *subj, void *user_data) {
    (void)subj;
    apply_vfo_agc((radio_vfo_t)user_data);
}

static void on_cw_agc_cond_change(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    apply_vfo_agc(RADIO_VFO_A);
    apply_vfo_agc(RADIO_VFO_B);
}

static void update_agc_time(Subject *subj, void *user_data) {
    radio_agc_t     agc = cparam_i_get(cfg_cur_agc);
    radio_mode_t    mode = cparam_i_get(cfg_cur_mode);
    uint16_t        agc_time = 500;

    switch (agc) {
        case radio_agc_off:
            agc_time = 1000;
            break;

        case radio_agc_slow:
            agc_time = 1000;
            break;

        case radio_agc_fast:
            agc_time = 100;
            break;

        case radio_agc_auto:
            switch (mode) {
                case radio_mode_lsb:
                case radio_mode_lsb_dig:
                case radio_mode_usb:
                case radio_mode_usb_dig:
                    agc_time = 500;
                    break;

                case radio_mode_cw:
                case radio_mode_cwr:
                    agc_time = 100;
                    break;

                case radio_mode_am:
                case radio_mode_nfm:
                case radio_mode_sam:
                case radio_mode_wfm:
                    agc_time = 1000;
                    break;
            }
            break;
    }
    radio_lock();
    bool changed = hw_set_i(HW_AGC_TIME, agc_time);
    radio_unlock();
    if (changed) {
        LV_LOG_USER("Radio set agc time=%u for agc: %i\n", agc_time, agc);
    }
}

static void on_vfo_att_change(Subject *subj, void *user_data) {
    radio_vfo_t vfo = (radio_vfo_t )user_data;
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    WITH_RADIO_LOCK(hw_vfo_set_att(vfo, new_val));
    LV_LOG_USER("Radio set vfo %i att=%i", vfo, new_val);
}

static void on_vfo_pre_change(Subject *subj, void *user_data) {
    radio_vfo_t vfo = (radio_vfo_t )user_data;
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    WITH_RADIO_LOCK(hw_vfo_set_pre(vfo, new_val));
    LV_LOG_USER("Radio set vfo %i pre=%i", vfo, new_val);
}

static void on_atu_network_change(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    uint32_t new_val = cfg_atu_get_network();
    WITH_RADIO_LOCK(hw_set_i(HW_ATU_NETWORK, new_val));
    LV_LOG_USER("Radio set atu network=%u", new_val);
}

static void on_low_filter_change(Subject *subj, void *user_data) {
    radio_mode_t mode = cparam_i_get(cfg_cur_mode);
    if (mode == radio_mode_am || mode == radio_mode_nfm || mode == radio_mode_sam || mode == radio_mode_wfm) {
        // No update low on AM/FM, low should be -high
        return;
    }
    int32_t low = cparam_i_get(cfg_cur_filter_low);
    int32_t low2 = LV_MAX(0, low - FILTER_2_OFFSET_OUT);
    radio_lock();
    LV_LOG_USER("Radio set filters: low=%i, low2=%i", low, low2);
    hw_set_i(HW_FILTER1_LOW, low);
    hw_set_i(HW_FILTER2_LOW, low2);
    radio_unlock();
}

static void on_high_filter_change(Subject *subj, void *user_data) {
    int32_t high = cparam_i_get(cfg_cur_filter_high);
    int32_t high2 = high + FILTER_2_OFFSET_OUT;
    switch (cparam_i_get(cfg_cur_mode)) {
        case radio_mode_am:
        case radio_mode_nfm:
        case radio_mode_sam:
        case radio_mode_wfm:
            // For AM BASE uses absolute low and high values to choose LPF filters for I and Q
            radio_lock();
            LV_LOG_USER("Radio set filters: low=%i, low2=%i", -high, -high2);
            hw_set_i(HW_FILTER1_LOW, -high);
            hw_set_i(HW_FILTER2_LOW, -high2);
            radio_unlock();
            break;
        default:
            break;
    }
    radio_lock();
    LV_LOG_USER("Radio set filters: high=%i, high2=%i", high, high2);
    hw_set_i(HW_FILTER1_HIGH, high);
    hw_set_i(HW_FILTER2_HIGH, high2);
    radio_unlock();
}

static void on_change_comp_ratio(Subject *subj, void *user_data) {
    uint8_t ratio = subject_i_get((SubjectInt*)subj);
    if (ratio < 1) {
        ratio = 1;
    }
    if (ratio == 1) {
        // invert
        WITH_RADIO_LOCK(hw_set_i(HW_COMP_ON, true));
    } else {
        radio_lock();
        hw_set_i(HW_COMP_ON, false);
        hw_set_i(HW_COMP_LEVEL, ratio - 2);
        radio_unlock();
    }
}

static void on_fw_zoom_change(Subject *subj, void *user_data) {
    uint8_t zoom = subject_i_get((SubjectInt*)subj);
    uint8_t val = 0;
    while (zoom > 1) {
        val++;
        zoom >>= 1;
    }
    WITH_RADIO_LOCK(hw_set_i(HW_FFT_DEC, val));
}

static void on_if_shift_change(Subject *subj, void *user_data) {
    int32_t shift = subject_i_get((SubjectInt*)subj);
    int32_t cur_freq = cparam_i_get(cfg_fg_freq);

    LV_LOG_USER("Shift: %d, cur_freq: %d\n", shift, cur_freq);
    if (shift != 0) {
        radio_lock();
        hw_set_i(HW_IF_SHIFT_HZ, shift);
        hw_set_i(HW_IF_SHIFT_ON, true);
        radio_unlock();
    } else {
        WITH_RADIO_LOCK(hw_set_i(HW_IF_SHIFT_ON, false));
    }
    radio_vfo_t vfo = param_i_get(cfg_band_current_vfo);
    on_vfo_freq_change(NULL, (void*)vfo);
}

static void on_band_change(Subject *subj, void *user_data) {
    // Workaround for bug with ignored IQ offset on band change from BASE
    int32_t i_val = param_i_get(cfg_band_tx_i_offset);
    int32_t q_val = param_i_get(cfg_band_tx_q_offset);
    if (i_val == hw_get_i(HW_TX_I_OFFSET)) {
        i_val++;
    }
    WITH_RADIO_LOCK(hw_set_i(HW_TX_I_OFFSET, i_val));
    if (q_val == hw_get_i(HW_TX_Q_OFFSET)) {
        q_val++;
    }
    WITH_RADIO_LOCK(hw_set_i(HW_TX_Q_OFFSET, q_val));
}

/**
 * Restore "listening" of main board and USB soundcard after ATU
 */
static void recover_processing_audio_inputs() {
    usleep(10000);
    radio_vfo_t vfo = param_i_get(cfg_band_current_vfo);
    radio_lock();
    hw_vfo_set_mode(vfo, radio_mode_usb_dig);
    hw_set_f(HW_TXPWR_F, 0.1f);
    hw_set_i(HW_MODEM, true);
    usleep(50000);
    hw_set_i(HW_MODEM, false);
    set_txpwr(param_f_get(cfg_pwr));
    hw_vfo_set_mode(vfo, cparam_i_get(cfg_cur_mode));
    radio_unlock();
}

/* Battery log for the discharge curve of the pack and the time left (bat_est.c), every
 * minute: the base gives the voltage (0.1 V steps) and its own %, no current. Called about
 * every 0.4 s: TX seconds and watts add up in between */
#define BATTERY_LOG     BOARD_DATA "/battery_log.csv"
#define BATTERY_MODEL   BOARD_DATA "/battery_model.json"
#define BATTERY_HEADER  "time,vbat,batcap,vext,charging,tx,backlight,cpu_mhz,tx_s,tx_w,wifi,bt,hp\n"

/* GUI thread: arg = { % , minutes left or -1 }. Short texts: longer ones scroll */
static void battery_warn_cb(void *arg) {
    int *w = arg;

    if (w[0] <= 5) {
        msg_schedule_long_text_fmt("Battery %d%%: connect power", w[0]);
    } else if (w[1] >= 0) {
        msg_schedule_long_text_fmt("Battery %d%%: about %d min left", w[0], w[1]);
    } else {
        msg_schedule_long_text_fmt("Battery %d%%", w[0]);
    }
}

static void battery_log(const hw_pack_t *p) {
    static time_t   last;
    static uint64_t prev_ms;
    static float    tx_s, tx_ws;
    static int      rows;
    time_t          t = time(NULL);
    struct stat     st;
    unsigned        mhz = 0;
    FILE            *f;
    char            head[sizeof(BATTERY_HEADER)];

    if (p->tx && prev_ms && now_time > prev_ms) {
        float dt = (now_time - prev_ms) / 1000.0f;

        dt = dt > 1 ? 1 : dt;
        tx_s += dt;
        tx_ws += p->tx_power * 0.1f * dt;
    }
    prev_ms = now_time;
    if (t - last < 60) {
        return;
    }
    /* At start the log is fed again: the stretch on battery goes on across a GUI restart (the
     * model learns only what is newer than it). No model yet: learn from the old log too */
    if (!last) {
        if (!bat_est_load(BATTERY_MODEL)) {
            bat_est_replay(BATTERY_LOG ".1");
        }
        bat_est_replay(BATTERY_LOG);
        bat_est_save(BATTERY_MODEL);
    }
    last = t;
    if ((f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r"))) {
        if (fscanf(f, "%u", &mhz) == 1) {
            mhz /= 1000;
        }
        fclose(f);
    }
    bat_sample_t s = {
        .t = t, .vbat = p->vbat * 0.1f, .cap = p->batcap, .vext = p->vext * 0.1f, .charging = p->charging,
        .tx_s = tx_s, .tx_w = tx_s > 0 ? tx_ws / tx_s : 0, .bl = backlight_level(),
        .wifi = wifi_get_status() != WIFI_OFF,
        .bt = access("/sys/class/bluetooth/hci0", F_OK) == 0,
        .hp = access("/tmp/btaudio-hp", F_OK) == 0,
    };
    bool chg, approx;

    tx_s = tx_ws = 0;
    bat_est_feed(&s);
    int min = bat_est_minutes(0, &chg, &approx);

    clock_update_estimate(min, approx);

    /* On battery at 10% or less: a warning every minute (the 30 s power-off comes at 6.0 V) */
    if (s.vext < 3.0f && s.vbat >= 3.0f && p->batcap <= 10) {
        int w[2] = { p->batcap, min };

        scheduler_put(battery_warn_cb, w, sizeof(w));
    }
    if (++rows % 5 == 0) {      /* a GUI restart loses at most 5 minutes of learning */
        bat_est_save(BATTERY_MODEL);
    }

    /* A new file past 2 MB (about 3 weeks), or when it still has the old columns */
    bool fresh = stat(BATTERY_LOG, &st) != 0;

    if (!fresh && rows == 1 && (f = fopen(BATTERY_LOG, "r"))) {
        bool same = fgets(head, sizeof(head), f) && strcmp(head, BATTERY_HEADER) == 0;

        fclose(f);
        if (!same) {
            st.st_size = INT32_MAX;
        }
    }
    if (!fresh && st.st_size > 2 * 1024 * 1024) {
        rename(BATTERY_LOG, BATTERY_LOG ".1");
        fresh = true;
    }
    if (!(f = fopen(BATTERY_LOG, "a"))) {
        return;
    }
    if (fresh) {
        fputs(BATTERY_HEADER, f);
    }
    fprintf(f, "%ld,%.1f,%u,%.1f,%u,%u,%d,%u,%.0f,%.1f,%u,%u,%u\n", (long) t, s.vbat, p->batcap, s.vext,
            p->charging, s.tx_s > 0, s.bl, mhz, s.tx_s, s.tx_w, s.wifi, s.bt, s.hp);
    fclose(f);
}

static swr_guard_t swr_guard;

/* SWR protection, on the GUI thread: what the GUI keys lets go (a PTT held on the radio or the
 * mic, VOX and the radio's own keyer are the base's: they may key again) */
static void swr_trip_cb(void *arg) {
    cw_encoder_stop();
    paddle_keyer_stop();
    dialog_msg_voice_ptt_pressed();
    radio_set_morse_key(false);
    radio_set_modem(false);     /* the digital apps see it and stop (tx_player.c) */
    radio_set_ptt(false);
    msg_schedule_text_fmt("TX stopped: SWR %.1f", *(float *)arg);
}

int radio_get_battery() {
    return bat_pct;
}

static bool radio_tick() {
    if (now_time < prev_time) {
        prev_time = now_time;
    }

    int32_t d = now_time - prev_time;

    if (hw_flow_read(pack)) {
        prev_time = now_time;
        pack_time = now_time;
        pack_vbat = pack->vbat;
        pack_vext = pack->vext;

        static uint8_t delay;

        if (delay++ > 10) {
            delay = 0;
            clock_update_power(pack->vext * 0.1f, pack->vbat*0.1f, pack->batcap, pack->charging);
            battery_log(pack);
            /* Only on a change, with hysteresis: around 6.0 V the 30 s
             * power-off timer was started and stopped over and over */
            /* External power: the flag alone is not enough, charging an
             * empty battery it stayed clear with 11.7 V on the jack and the
             * radio switched itself off on the charger */
            bool ext = pack->vext_flag || pack->charging || pack->vext >= 90;
            /* At power-on the base sends a few packets before it has measured anything (0 V on both):
             * one of them alone started "Low battery!" with a full battery on the charger. Low only
             * after 3 readings in a row (about 1.2 s), and never on a reading of nothing */
            static uint8_t low_reads;
            bool           measured = pack->vbat > 0 || pack->vext > 0;

            /* The % sags under the TX current and comes back slowly: the last RX reading holds
             * while transmitting and for 30 s after (also in the top bar, clock.c) */
            static uint64_t hold_until;
            static int8_t   held_cap = -1;
            int8_t          cap = measured && !ext ? pack->batcap : -1;

            if (radio_get_state() == RADIO_TX) hold_until = now_time + 30000;
            if (now_time < hold_until && held_cap >= 0 && cap >= 0) cap = held_cap;
            else held_cap = cap;
            bat_pct = cap;   /* the % on the screen (clock.c) */
            if (cap >= 0 && cap != pack->batcap)
                clock_update_power(pack->vext * 0.1f, pack->vbat * 0.1f, cap, pack->charging);

            low_reads = measured && !ext && pack->vbat <= LOW_BATTERY_VBAT_LOW ? (low_reads < 3 ? low_reads + 1 : 3) : 0;
            bool enter_low = low_reads >= 3;
            bool leave_low = ext || (pack->vbat >= LOW_BATTERY_VBAT_HIGH);

            if ((!low_power && enter_low) || (low_power && leave_low)) {
                low_power = !low_power;
                scheduler_put(low_power_ui_cb, &low_power, sizeof(low_power));
            }
        }
        if (pack->psd) {
            dsp_psd(pack->psd, pack->n_psd, pack->psd_span, pack->tx, pack->smeter_dbm);
        } else {
            dsp_samples((cfloat *)pack->samples, pack->n_samples, pack->tx, pack->lo_freq, pack->vary_freq, pack->fft_dec);
        }

        /* The POWER key where the base reports it (the X6200): to the GUI thread as a key */
        static bool power_was;

        if (pack->power_key != power_was) {
            power_was = pack->power_key;
            scheduler_put(power_key_cb, &power_was, sizeof(power_was));
        }

        switch (state) {
            case RADIO_RX:
                if (pack->tx && state_move(RADIO_RX, RADIO_TX)) {
                    if (notify_rx_tx) {
                        notify_rx_tx(true);
                    }
                }
                break;

            case RADIO_TX:
                if (!pack->tx && state_move(RADIO_TX, RADIO_RX)) {
                    if (notify_rx_tx) {
                        notify_rx_tx(false);
                    }
                } else {
                    // printf("%d, %d\n", pack->tx_power, pack->alc_level);
                    tx_info_update(pack->tx_power * 0.1f, pack->vswr * 0.1f, pack->alc_level * 0.1f);
                    float swr = pack->vswr * 0.1f;
                    if (swr_guard_check(&swr_guard, pack->tx_power * 0.1f, swr, param_i_get(cfg_swr_protect) * 0.1f, now_time)) {
                        scheduler_put(swr_trip_cb, &swr, sizeof(swr));
                    }
                }
                break;

            case RADIO_ATU_START:
                WITH_RADIO_LOCK(hw_set_i(HW_ATU_TUNE, true));
                state_move(RADIO_ATU_START, RADIO_ATU_WAIT);
                break;

            case RADIO_ATU_WAIT:
                if (pack->tx) {
                    if (notify_rx_tx) {
                        notify_rx_tx(true);
                    }
                    state_move(RADIO_ATU_WAIT, RADIO_ATU_RUN);
                }
                break;

            case RADIO_ATU_RUN:
                if (pack->atu_status && !pack->tx) {
                    /* The settings on the LVGL thread: their observers run where they are set, and
                     * the delayed ones (the info bar) schedule LVGL calls, which only that thread may */
                    uint32_t network = pack->atu_params;
                    scheduler_put(atu_done_cb, &network, sizeof(network));
                    WITH_RADIO_LOCK(hw_set_i(HW_ATU_TUNE, false));
                    recover_processing_audio_inputs();
                    if (notify_rx_tx) {
                        notify_rx_tx(false);
                    }

                    // TODO: change with observer on atu->loaded change
                    WITH_RADIO_LOCK(hw_set_i(HW_ATU_NETWORK, pack->atu_params));
                    state_move(RADIO_ATU_RUN, RADIO_RX);
                } else if (pack->tx) {
                    tx_info_update(pack->tx_power * 0.1f, pack->vswr * 0.1f, pack->alc_level * 0.1f);
                }
                break;

            case RADIO_SWRSCAN:
                dialog_swrscan_update(pack->vswr * 0.1f);
                break;

            case RADIO_POWEROFF:
                WITH_RADIO_LOCK(hw_poweroff());
                state_move(RADIO_POWEROFF, RADIO_OFF);
                break;

            case RADIO_OFF:
                break;
        }

        bool gui_tx = (state != RADIO_RX && state != RADIO_TX)
            || hw_tx_keyed_by_gui()
            || morse_on || (uint32_t)now_time - morse_ms < MORSE_HANG_MS;

        if (tx_watch_check(&tx_watch, now_time, pack->tx, gui_tx)) {
            base_guard_log("base still in TX %u ms after an un-key or write error, nothing keying it from the GUI: PTT off resent",
                           TX_WATCH_HOLD_MS);
            resync_due = true;
        }

        switch (tx_watch_power(&tx_watch, now_time, pack->tx)) {
            case TX_WATCH_CUT:
                WITH_RADIO_LOCK(hw_set_f(HW_TXPWR_F, 0.0f));
                base_guard_log("base still in TX %u ms after PTT off and a write error: TX power set to 0 (vext %.1f V, vbat %.1f V)",
                               TX_WATCH_CUT_MS, pack->vext * 0.1f, pack->vbat * 0.1f);
                break;
            case TX_WATCH_RESTORE:
                WITH_RADIO_LOCK(set_txpwr(param_f_get(cfg_pwr)));
                base_guard_log("base back in RX: TX power %.1f W restored (vext %.1f V, vbat %.1f V)",
                               param_f_get(cfg_pwr), pack->vext * 0.1f, pack->vbat * 0.1f);
                break;
        }

        /* The hand mic key to the LVGL thread: hkey_put makes and deletes LVGL timers, which
         * only that thread may do. Posted when it changes (held down, the base repeats it) */
        static uint32_t last_hkey;
        if (pack->hkey != last_hkey) {
            last_hkey = pack->hkey;
            scheduler_put(hkey_sched_cb, &last_hkey, sizeof(last_hkey));
        }
    } else {
        if (d > FLOW_RESTART_TIMEOUT) {
            LV_LOG_WARN("Flow reset");
            static uint64_t silent_logged;
            if (base_silent_due(now_time, start_time, &silent_logged)) {
                char vb[8] = "n/a", ve[8] = "n/a";
                if (pack_vbat) {
                    snprintf(vb, sizeof(vb), "%.1f V", pack_vbat * 0.1f);
                }
                if (pack_vext) {
                    snprintf(ve, sizeof(ve), "%.1f V", pack_vext * 0.1f);
                }
                base_guard_log("base silent for %u ms, battery %s, external %s",
                               (unsigned)(now_time - pack_time), vb, ve);
            }
            prev_time = now_time;
            hw_flow_restart();
            dsp_reset();
        }
        return true;
    }
    return false;
}

/* Write errors and un-keys seen by base_guard.c: arm the TX watch, resend the registers */
static void guard_events() {
    unsigned resent, lost;
    int      reg;
    unsigned ev = base_guard_take(&resent, &lost, &reg);

    if (ev & BASE_GUARD_ERROR) {
        base_guard_log("write to register %d failed: %u resent OK, %u lost; resending all registers", reg, resent, lost);
        tx_watch_error(&tx_watch, now_time);
    }
    if (ev & BASE_GUARD_UNKEY) {
        tx_watch_arm(&tx_watch, now_time, TX_WATCH_UNKEY_MS);
    }
    if (ev) {
        resync_due = true;
    }
}

/* The whole register block (the library's copy) to the base; false: it didn't get through.
 * The library reopens the bus and tries once more on its own, and reports nothing but errno */
static bool resend_all() {
    radio_lock();
    bool ok = hw_resend_all();
    radio_unlock();
    return ok;
}

static void * radio_thread(void *arg) {
    while (true) {
        now_time = get_time();

        guard_events();

        if (radio_tick()) {
            usleep(2000);
        }

        int32_t idle = now_time - idle_time;

        /* Every 3 s in RX as always; after a write error or an un-key at once, in TX too */
        if ((idle > IDLE_TIMEOUT && state == RADIO_RX) || (resync_due && now_time - resync_time >= RESYNC_RETRY_MS)) {
            resync_time = now_time;
            resync_due = !resend_all();
            if (resync_due) {
                base_guard_log("register block not sent, again in %u ms", RESYNC_RETRY_MS);
                tx_watch_error(&tx_watch, now_time);
            }
            idle_time = now_time;
        }
    }
    return NULL;
}
