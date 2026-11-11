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
#include <stdarg.h>
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
#include "tune_tx.h"
#include "paddle_keyer.h"
#include "cw_encoder.h"
#include "dialog_msg_voice.h"
#include "msg.h"
#include "base_guard.h"
#include "audio.h"
#include "bat_est.h"
#include "bat_watch.h"
#include "start_trace.h"
#include "clock.h"
#include "wifi.h"
#include "settings_window.h"
#include "wr12.h"
#include "pa_bias.h"
#include "freq_strip.h"

/*********************
 *      DEFINES
 *********************/

#define FLOW_RESTART_TIMEOUT 300
#define IDLE_TIMEOUT        (3 * 1000)
#define KEEPALIVE_MS        1000    /* HW_CAP_KEEPALIVE: as gdyuldin's X6200 GUI, in TX too */
#define RESYNC_RETRY_MS     200     /* the register block again, after one that didn't get through */
#define MORSE_HANG_MS       3000    /* the base holds TX for the QSK time after the GUI's CW key */
#define ATU_START_MS        3000    /* the base keys for the tuner within this, or the tune is given up */
#define ATU_NO_MATCH_MS     1000    /* back in RX this long without the tuner's done flag: no match */
#define OPEN_TX_SETTLE_MS   500     /* the X6200's open TX goes this long after the dial's last move (open_tx_look), */
#define OPEN_TX_RETRY_MS    5000    /* again this long after one that failed, twice as long after each one more, */
#define OPEN_TX_RETRY_MAX_MS 60000  /* up to this */

#define FILTER_2_OFFSET 60
#define FILTER_2_OFFSET_IN 20
#define FILTER_2_OFFSET_OUT (FILTER_2_OFFSET - FILTER_2_OFFSET_IN)

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
static void on_transverter_change(Subject *subj, void *user_data);
static void on_xvtr_pwr_change(Subject *subj, void *user_data);
static void xvtr_settle_cb(void *arg);
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
static void on_rit_change(Subject *subj, void *user_data);
static void on_split_change(Subject *subj, void *user_data);
static void on_pwr_change(Subject *subj, void *user_data);
static void on_train_hold_change(Subject *subj, void *user_data);
static void on_band_hold_change(Subject *subj, void *user_data);
static void band_settle_cb(void *arg);
static void band_hold_set(bool hold);
static bool band_hold_early(radio_vfo_t vfo, int32_t freq);
static void band_look(bool settled);
static bool split_keys_dial(void);
static void split_held_look(void);
static void on_vox_change(Subject *subj, void *user_data);
static void on_key_train_change(Subject *subj, void *user_data);
static void on_volume_change(Subject *subj, void *user_data);
static bool wr12_takes(int32_t freq);
static void wr12_follow(bool rx, int32_t dial);
static void open_tx_look(void);
static void open_tx_key(void);
static void open_tx_cw_down(void);
static void open_tx_cw_wait(void);
static void open_tx_tick(void);

#ifndef BOARD_X6200
static void recover_processing_audio_inputs();
#else
static bool wr12_notify(void);
static bool wr12_keyed(void);
static void on_wr12_change(Subject *subj, void *user_data);
static void pa_bias_send(void);
#endif
/* Who keys (tx_refused): the CW Trainer's key goes through outside the amateur bands, at 0 W, its PTT quietly not */
typedef enum { TX_BY_GUI, TX_BY_PTT, TX_BY_MORSE } tx_by_t;
static bool tx_refused(tx_by_t by);
static bool tx_refused_at(int32_t freq, tx_by_t by);
#ifdef BOARD_PWR_MAX_BAT
static void tune_cap_top(float top);
#endif
static bool radio_tick();
static void * radio_thread(void *arg);

/* The tuner found its network, on the LVGL thread (see the flow handler) */
static void atu_done_cb(void *arg) {
    cfg_atu_save_network(*(uint32_t *)arg);
    param_i_set(cfg_atu_enabled, true);
}

/* The base's warnings, on the LVGL thread: bit 0 battery hot, 1 PA hot, 2 DC in too low,
 * 3 battery cold, shown in the words of Xiegu's app */
static void base_warn_cb(void *arg) {
    static const char *words[] = {"BATTERY HIGH TEMP!", "PA HIGH TEMP!", "DCIN TOO LOW!", "BATTERY LOW TEMP!"};
    uint8_t            w = *(uint8_t *)arg;
    char               text[96] = "";

    for (int i = 0; i < 4; i++) {
        if (w & 1 << i) {
            snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%s", text[0] ? "\n" : "", words[i]);
        }
    }
    msg_schedule_long_text_fmt("%s", text);
}

/* A hand mic key, on the LVGL thread (see the flow handler) */
static void hkey_sched_cb(void *arg) {
    hkey_put(*(uint32_t *)arg);
}

#ifdef BOARD_PWR_MAX_BAT
/* The supply changed (true: external power), on the LVGL thread. On battery the cap drops and a
 * power set above it comes down to it; back on external power only the cap goes up, the power
 * stays where it was, as in Xiegu's app */
static void supply_cb(void *arg) {
    if (*(bool *)arg) {
        cfg_pwr_max_set(BOARD_PWR_MAX);
    } else {
        cfg_pwr_max_set(BOARD_PWR_MAX_BAT);
        tune_cap_top(BOARD_PWR_MAX_BAT);
        if (param_f_get(cfg_pwr) > BOARD_PWR_MAX_BAT) {
            cfg_pwr_set(BOARD_PWR_MAX_BAT);
            msg_schedule_text_fmt("TX power %0.0f W on battery", BOARD_PWR_MAX_BAT);
        }
    }
}
#endif

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
static bool             base_seen;      /* a packet came in this run */
static atomic_uint_fast64_t data_time;  /* pack_time for the other threads (radio_no_data), 0 before the first packet */
static bool             gone_said;      /* "No data from the base" said, until a packet comes (base_gone) */
static bool             tx_by_gui;      /* the GUI keyed the TX under way (PTT, an app, the CW key): radio thread */
static uint8_t          pack_vbat, pack_vext;   /* its voltages, 0.1 V */
static volatile int8_t  bat_pct = -1;   /* the battery %, -1 on external power or not measured yet */
static volatile bool    charger_asked;  /* the charger setting as last applied (Shadow at power-off left out: no bolt then) */
static bool             mute = false;
static bool             tci_mute = false;   /* a TCI client transmits: the speaker and the monitor off */
static bool             scan_mute = false;  /* the scanner between stops: no noise burst at every step */
static bool             rec_moni = false;   /* a screen recording takes the TX audio: the monitor at 30 at least (X6100) */
static bool             digi_cap;       /* a digital app is open: never above its cap */
static float            tune_cap;       /* a TUNE for an external tuner: never above its power (under the lock) */
static atomic_int       tx_mode = -1;   /* radio_set_tx_mode's for the transmission under way, -1 none */
/* A transverter's TX power limit where the radio is tuned (#104): never above it, 0 none; -1 until first looked at
 * (on_xvtr_pwr_change). Under the lock, as txpwr_asked: the last power asked of set_txpwr, before its caps */
static float            xvtr_cap = -1.0f;
static float            txpwr_asked;
/* KEY Train in CW or CW-R (#127): the power at the base held at 0 W, whatever keys the radio. In Train the base's keyer
 * sounds only the sidetone with Auto-L and Auto-R (the X6200's base 1.0.8 goes into TX at 0.0 W), but in Manual its
 * KEY jack keyed the X6200 at PWR, Train or not. Written under the lock (on_train_hold_change), read by the radio
 * thread too; train_said: its note on the screen, once while it holds (train_note) */
static atomic_bool      train_hold;
static atomic_bool      train_said;
/* Where the radio may not transmit (#121: cfg_tx_refusal of radio_tx_freq; Birdie Cal off the dial): the power at the
 * base held at 0 W, its VOX off and its KEY Train on, for what the base keys by itself (the hand mic's PTT, VOX, the
 * KEY jack); what the GUI keys is refused there (tx_refused), or stopped (band_stop). Written under the lock
 * (band_hold_set), read by the radio thread too; band_said: its note, once while it holds (band_note); band_looked:
 * the first look made (radio_start), no early hold before it; off_dial: radio_set_freq moved a VFO of the base off
 * its dial, Birdie Cal's, held until radio_set_freq_dial at the run's end; band_stops: the GUI's TX stopped by it,
 * for tx_player; atu_band_stop: the radio's tuner to give up, for the radio thread */
static atomic_bool      band_hold;
static atomic_bool      band_said;
static bool             band_looked;
static atomic_bool      off_dial;
static atomic_uint      band_stops;
static atomic_bool      atu_band_stop;
static pthread_mutex_t  tune_mux = PTHREAD_MUTEX_INITIALIZER;
static tune_guard_t     tune_guard;     /* that tune, for the SWR protection (under tune_mux) */

static tx_watch_t       tx_watch;
static bool             resync_due;     /* resend the whole register block (base_guard.h) */
static uint64_t         resync_time;
static uint64_t         block_time;     /* the whole register block last sent (resend_all), radio thread only */
static volatile bool    morse_on;
static volatile uint32_t morse_ms;      /* the GUI's CW key last changed */
static atomic_uint      morse_down_ms;  /* when the GUI's CW key went down (get_time, never 0), 0: up */
static atomic_uint      morse_limit_ms; /* radio_set_morse_key_limit, 0 none */
static atomic_uint      morse_cuts;
static uint64_t         atu_time;       /* the tune asked, then the base last seen in TX for it */

static void power_key_cb(void *arg) {
    keypad_power_set(*(bool *)arg);
}

/* The SWR scan's carrier: 1 W is enough to read the SWR. Held here for as long as the scan runs: PWR set meanwhile
 * (TCI's drive, the Panel's PWR, the X6200's battery cap) came through on_pwr_change, and the sweep went on at PWR */
#define SWRSCAN_PWR_W   1.0f

static void set_txpwr(float w) {
    txpwr_asked = w;
    if (atomic_load(&state) == RADIO_SWRSCAN && w > SWRSCAN_PWR_W) w = SWRSCAN_PWR_W;
    /* An app open: its cap, but not over a TUNE's own power (#87), which holds the power to it instead */
    if (digi_cap && tune_cap <= 0.0f && w > TX_PLAYER_MAX_PWR_W) w = TX_PLAYER_MAX_PWR_W;
    if (tune_cap > 0.0f && w > tune_cap) w = tune_cap;     /* PWR turned up, the battery's top, a restore */
    if (xvtr_cap > 0.0f && w > xvtr_cap) w = xvtr_cap;     /* whoever asks: PWR, an app's own, TUNE's, TCI, CAT */
    /* A TUNE keeps its own power: the base is in USB-D for it, and the user asked for that carrier */
    if (atomic_load(&train_hold) && tune_cap <= 0.0f) w = 0.0f;
    if (atomic_load(&band_hold)) w = 0.0f;     /* a TUNE too: none starts there, one moved there is stopped */
    hw_set_f(HW_TXPWR_F, w);
}

/* Whether the base's charging flag counts: on the X6200 only when charging was asked for, as its base (1.0.8)
 * raises the flag with external power in whatever the setting (#33). The X6100's flag is taken as its base
 * gives it: nobody has seen that base raise it with the charger off */
static bool charge_flag_counts(void) {
#ifdef BOARD_X6200
    return charger_asked;
#else
    return true;
#endif
}

static uint32_t min_tx;
static uint32_t max_tx;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void radio_bb_reset() {
    hw_bb_reset();
}

bool radio_init() {
    if (!hw_init()) {
        start_trace("base: its pins or its line could not be opened");
        return false;
    }

    /* Back off (0.1 .. 25.6 s) and, once, after about 6 s without an answer,
     * pulse the baseband reset pin instead of waiting forever (upstream 37f3894) */
    uint8_t  cnt = 0;
    unsigned tries = 0;

    while (!hw_control_init()) {
        start_trace("base: no answer (try %u)", ++tries);
        usleep(100000 << cnt);
        if (cnt < 8) cnt++;
        if (cnt == 6) {
            fprintf(stderr, "Baseband not answering, reset\n");
            start_trace("base: reset");
            radio_bb_reset();
            usleep(1000000);
        }
    }
    start_trace("base answered");
    return true;
}

void radio_start() {

    hw_start();
    base_ver = hw_base_ver();
    cfg_hw_caps_set(hw_caps());     /* the LO offset from them, before the first frame */
    start_trace("base: version %s", hw_base_ver_str());
#ifdef BOARD_X6200
    /* The WR12 on the HOST port (wr12.c, #134): its thread, and WR12 on HOST, before the VFOs go to the base. While
     * it is out of its bypass, or may be, nothing transmits anywhere (cfg_tx_refusal), from the first look at the
     * hold below on: one a GUI before this one left out of it too */
    wr12_init(wr12_notify, wr12_keyed);
    cfg_tx_block_set(wr12_tx_block);
    subject_subscribe_and_notify((Subject*)cfg_wr12, on_wr12_change, NULL);
#endif
    /* What the digital TX level comes from, for a report of an app at 0 W (#16); the base version was nowhere */
    fprintf(stderr, "radio: base %s, line_in %u, line_out %u, play %.1f dB, rec %.1f dB, digital level %.1f dB\n",
            hw_base_ver_str(), params.line_in, params.line_out, params.play_gain_db_f.x, params.rec_gain_db_f.x,
            params.ft8_output_gain_offset.x);

    subject_subscribe_and_notify((Subject*)cfg_band_vfoa_freq, on_vfo_freq_change, (void*)RADIO_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg_band_vfob_freq, on_vfo_freq_change, (void*)RADIO_VFO_B);

    subject_subscribe((Subject*)cfg_transverter_0_from, on_transverter_change, NULL);
    subject_subscribe((Subject*)cfg_transverter_0_to, on_transverter_change, NULL);
    subject_subscribe((Subject*)cfg_transverter_0_shift, on_transverter_change, NULL);
    subject_subscribe((Subject*)cfg_transverter_1_from, on_transverter_change, NULL);
    subject_subscribe((Subject*)cfg_transverter_1_to, on_transverter_change, NULL);
    subject_subscribe((Subject*)cfg_transverter_1_shift, on_transverter_change, NULL);

    /* A transverter's TX power limit (#104) follows where the radio listens and transmits and the transverters'
     * settings; looked at once now, before PWR goes to the base below */
    Subject *xvtr_by[] = {
        (Subject*)cfg_fg_freq, (Subject*)cfg_bg_freq, (Subject*)cfg_band_split, (Subject*)cfg_band_tx_shift,
        (Subject*)cfg_transverter_0_from, (Subject*)cfg_transverter_0_to, (Subject*)cfg_transverter_0_shift,
        (Subject*)cfg_transverter_0_pwr, (Subject*)cfg_transverter_1_from, (Subject*)cfg_transverter_1_to,
        (Subject*)cfg_transverter_1_shift, (Subject*)cfg_transverter_1_pwr,
    };
    for (size_t i = 0; i < sizeof(xvtr_by) / sizeof(xvtr_by[0]); i++) {
        subject_subscribe(xvtr_by[i], on_xvtr_pwr_change, NULL);
    }
    on_xvtr_pwr_change(NULL, NULL);

    /* Outside the amateur bands the base held at 0 W, VOX off, KEY Train on (#121): where the radio transmits and the
     * switch. Looked at once now, before PWR, VOX and Train go to the base below; subscribed before the split bit and
     * the VFO in use (HW_SPLIT, HW_VFO_CUR below), which observers get in that order: held before the base moves */
    Subject *band_by[] = {
        (Subject*)cfg_fg_freq, (Subject*)cfg_bg_freq, (Subject*)cfg_band_split, (Subject*)cfg_band_tx_shift,
        (Subject*)cfg_band_current_vfo, (Subject*)cfg_transverter_0_from, (Subject*)cfg_transverter_0_to,
        (Subject*)cfg_transverter_0_shift, (Subject*)cfg_transverter_1_from, (Subject*)cfg_transverter_1_to,
        (Subject*)cfg_transverter_1_shift, (Subject*)cfg_wr12, (Subject*)cfg_tx_outside_bands,
    };
    for (size_t i = 0; i < sizeof(band_by) / sizeof(band_by[0]); i++) {
        subject_subscribe(band_by[i], on_band_hold_change, NULL);
    }
    on_band_hold_change(NULL, NULL);

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
#ifdef BOARD_X6200
    subject_subscribe((Subject*)cfg_band_current_vfo, on_wr12_change, NULL);  /* once the base has it: the WR12 follows */
#endif
    subject_subscribe_and_notify((Subject*)cfg_band_split, on_change_i, (void*)HW_SPLIT);
    subject_subscribe((Subject*)cfg_band_split, on_split_change, NULL);
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
    subject_subscribe((Subject*)cfg_speaker_off, on_volume_change, NULL);   /* Speaker always off */
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
    subject_subscribe_and_notify((Subject*)cfg_vox_on, on_vox_change, NULL);
    subject_subscribe_and_notify((Subject*)cfg_vox_gain, on_change_i, (void*)HW_VOX_GAIN);
    subject_subscribe_and_notify((Subject*)cfg_vox_ag, on_change_i, (void*)HW_VOX_AG);
    subject_subscribe_and_notify((Subject*)cfg_vox_delay, on_change_i, (void*)HW_VOX_DELAY);

    subject_subscribe_and_notify((Subject*)cfg_mic, on_change_i, (void*)HW_MIC);
    subject_subscribe_and_notify((Subject*)cfg_hmic, on_change_i, (void*)HW_HMIC);
    subject_subscribe_and_notify((Subject*)cfg_imic, on_change_i, (void*)HW_IMIC);
    subject_subscribe_and_notify((Subject*)cfg_moni, on_volume_change, NULL);   /* radio_vol_refresh: muted with the speaker */

    subject_subscribe_and_notify((Subject*)cfg_rit, on_rit_change, NULL);
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
    subject_subscribe_and_notify((Subject*)cfg_key_train, on_key_train_change, NULL);
    subject_subscribe_and_notify((Subject*)cfg_qsk_time, on_change_i, (void*)HW_QSK_TIME);
    subject_subscribe_and_notify((Subject*)cfg_key_ratio, on_change_f, (void*)HW_KEY_RATIO_F);
    subject_subscribe_and_notify((Subject*)cfg_cw_peak_on, on_change_i, (void*)HW_CW_PEAK);
    subject_subscribe_and_notify((Subject*)cfg_cw_peak_q, on_change_i, (void*)HW_CW_PEAK_Q);

    /* KEY Train holds the power at 0 W in CW and CW-R (set_txpwr): looked at again when Train, a VFO's mode, the VFO
     * in use or split changes (a band key, a memory, CAT and TCI change the mode through them) */
    Subject *train_by[] = {
        (Subject*)cfg_key_train, (Subject*)cfg_band_vfoa_mode, (Subject*)cfg_band_vfob_mode,
        (Subject*)cfg_band_current_vfo, (Subject*)cfg_band_split,
    };
    for (size_t i = 0; i < sizeof(train_by) / sizeof(train_by[0]); i++) {
        subject_subscribe(train_by[i], on_train_hold_change, NULL);
    }
    on_train_hold_change(NULL, NULL);

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

    charger_asked = params.charger.x == RADIO_CHARGER_ON;
    hw_set_i(HW_CHARGER, charger_asked);
#ifdef BOARD_X6200
    pa_bias_send();
#else
    hw_set_i(HW_BIAS_DRIVE, params.bias_drive);
    hw_set_i(HW_BIAS_FINAL, params.bias_final);
#endif
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
    bool    off;

    /* The base's VFO in use moved without the dial (Birdie Cal, #121): held at 0 W, VOX off, for the whole run (the
     * CB bands are in the band list), let go by radio_set_freq_dial only. Not when the knob puts that VFO back: with
     * the VFOs swapped meanwhile the other one stays where the run had it, the one an X6100 in split keys */
    radio_lock();
    off = freq != cparam_i_get(cfg_fg_freq);
    if (off) {
        atomic_store(&off_dial, true);
        band_hold_set(true);
    }
    hw_vfo_set_freq(vfo, freq - shift);
    if (off) {
        wr12_follow(false, freq);   /* the base's own receiver for the run: the X6200's WR12 in its bypass (#134) */
    }
    radio_unlock();
    if (!off) band_look(true);
}

/* Birdie Cal's end: both VFOs of the base on their own frequency as it is now, IF shift and all, as
 * radio_set_tx_freq(0) does. The knob, CAT or a spot may have moved one meanwhile, and a VFO swapped during the run
 * left the other one where radio_set_freq had it, unseen by the hold (radio_tx_freq reads the settings) and keyed
 * there by the base after a swap back or in split. Then the hold off the dial goes; outside the bands it stays */
void radio_set_freq_dial(void) {
    on_vfo_freq_change(NULL, (void *)RADIO_VFO_A);
    on_vfo_freq_change(NULL, (void *)RADIO_VFO_B);
    atomic_store(&off_dial, false);
#ifdef BOARD_X6200
    /* Where the WR12 receives, the base back on its IF and the WR12 out of its bypass, now that the run is over */
    on_vfo_freq_change(NULL, (void *)(intptr_t)param_i_get(cfg_band_current_vfo));
#endif
    band_look(true);
}

bool radio_check_freq(int32_t freq) {
    return cfg_is_valid_hw_freq(freq);
}

/* A VFO's frequency from its own param, in_use: the dial's. The param is set before its observers run, cfg_fg_freq
 * and cfg_bg_freq only after them (ComputedParameter::set): a dial set through cfg_fg_freq read as the old one
 * meanwhile, in on_vfo_freq_change and on an app's thread, and a hold went on the old one's word (#121) */
static int32_t vfo_freq(bool in_use) {
    bool a = param_i_get(cfg_band_current_vfo) == RADIO_VFO_A;

    return param_i_get(a == in_use ? cfg_band_vfoa_freq : cfg_band_vfob_freq);
}

/* With a TX shift the frequency the shift asks for, not the other VFO's: that VFO is held to 100 kHz and up, so a
 * shift below 0 Hz left it at 100 kHz, which can transmit and keyed there. This one is refused (tx_refused) and red
 * on the screen instead */
int32_t radio_tx_freq(void) {
    int32_t shift = param_i_get(cfg_band_tx_shift);
    int32_t fg = vfo_freq(true);

    if (!param_i_get(cfg_band_split)) {
        return fg;
    }
    if (shift) {
        return param_i_get(cfg_band_current_vfo) == RADIO_VFO_A ? fg + shift : fg - shift;
    }
    return vfo_freq(false);
}

/* The base's split bit off while the active VFO carries the TX frequency (radio_set_tx_freq). cur_on_tx: the base's
 * VFO in use on radio_set_tx_freq's frequency, not the dial, until on_vfo_freq_change sends it its own again (the
 * knob turned in an app's slot, the slot's end) */
static atomic_bool split_held;
static atomic_bool cur_on_tx;
/* The X6200's PTT in split (ptt_moves) has the VFO in use on the TX frequency until it lets go, whoever moved it there:
 * what ends under it (a voice message, an app's slot, the SWR scan's window) does not take that VFO back to the dial,
 * nor does the knob or anything else that sends the base that VFO (on_vfo_freq_change). Set before the move */
static atomic_bool ptt_moved;

/* In split, whether the base keys the dial, the VFO in use (#121). The X6200's base does whatever split says: 1.0.8
 * ignores its split bit and has no TX VFO (2026-10-26: the GUI's CW spotted by RBN on VFO A with B 3 kHz up). Either
 * base does while split is held off, unless radio_set_tx_freq has that VFO on the TX frequency. Otherwise the other
 * VFO keys (radio_tx_freq). Out of split the dial is radio_tx_freq */
static bool split_keys_dial(void) {
    if (!param_i_get(cfg_band_split)) {
        return false;
    }
    if (atomic_load(&split_held)) {
        return !atomic_load(&cur_on_tx);
    }
#ifdef BOARD_X6200
    return true;
#else
    return false;
#endif
}

/* The X6200's PTT in split: the VFO in use goes to the TX frequency for it (radio_set_ptt), so the dial does not key */
static bool ptt_moves(void) {
#ifdef BOARD_X6200
    return param_i_get(cfg_band_split);
#else
    return false;
#endif
}

static bool key_refused(int32_t *freq, bool keys_dial) {
    int32_t f = radio_tx_freq();
    bool    wr12 = wr12_takes(vfo_freq(true));  /* the base on the X6200's WR12's IF, or on its way: nothing keys (#134) */

    if (wr12 || (!cfg_tx_refusal(f) && keys_dial)) {
        f = vfo_freq(true);
    }
    if (freq) {
        *freq = f;
    }
    return wr12 || cfg_tx_refusal(f) != NULL;
}

bool radio_key_refused(int32_t *freq) {
    return key_refused(freq, split_keys_dial());
}

bool radio_ptt_refused(int32_t *freq) {
    return key_refused(freq, split_keys_dial() && !ptt_moves());
}

int32_t radio_key_freq(void) {
    return split_keys_dial() ? vfo_freq(true) : radio_tx_freq();
}

/* The VFO the radio transmits on to `freq`. In split that is the active VFO, with the base's split bit off until
 * radio_set_tx_freq(0): the X6200's base (1.0.8) sent the apps' tone from VFO A whatever B said, an FT8 CQ at
 * 1200 Hz with a +700 Hz TX shift on 14.074 heard at 14.075325 (A + 1325, PSK Reporter, 2026-10-26); moving the
 * active VFO without split is the path both bases take. radio_set_freq moved the active one in split, so FT8 and the
 * other apps sent their tone at B + 1325 Hz, and never restored the IF shift (the X6100's VFO stayed off by it) */
void radio_set_tx_freq(int32_t freq) {
    radio_vfo_t vfo = param_i_get(cfg_band_current_vfo);

    if (freq == 0 && atomic_load(&ptt_moved)) {
        freq = radio_tx_freq();     /* over under PTT: its TX frequency, the dial when it lets go */
        if (cfg_tx_refusal(freq)) {
            return;     /* not to where TX is refused: it stays, held at 0 W, until the stop lets PTT go (band_stop) */
        }
    }
    if (freq == 0) {
        /* Both VFOs on their own frequency, transverter and IF shift as tuned: the one that transmits can have
         * changed since (split off during an SWR scan left B on a sweep point); one that did not move is not resent.
         * split_held until the bit is back: either base keys the VFO in use until then, and the dial it goes back to
         * is held where the radio may not transmit (band_hold_early, #121) */
        on_vfo_freq_change(NULL, (void *)RADIO_VFO_A);
        on_vfo_freq_change(NULL, (void *)RADIO_VFO_B);
        if (atomic_exchange(&split_held, false)) {
            on_train_hold_change(NULL, NULL);   /* the other VFO transmits again: its CW held, before the bit */
            WITH_RADIO_LOCK(hw_set_i(HW_SPLIT, param_i_get(cfg_band_split)));
            split_held_look();  /* on the X6100 the other VFO keys again: the dial's hold goes */
        }
        return;
    }
    if (!cfg_is_tx_freq(freq)) {    /* the hardware's check: the bands are tx_refused's, at the key */
        LV_LOG_ERROR("TX freq %i incorrect", freq);
        return;
    }

    bool held = param_i_get(cfg_band_split) && !atomic_exchange(&split_held, true);

    if (held) {
        split_held_look();  /* from the bit on the base keys the VFO in use, still on the dial: held there first */
        WITH_RADIO_LOCK(hw_set_i(HW_SPLIT, false));
        on_train_hold_change(NULL, NULL);   /* this VFO transmits now: the other one's CW holds nothing */
    }
    radio_lock();
    hw_vfo_set_freq(vfo, freq - cfg_transverter_shift_for(freq));
    atomic_store(&cur_on_tx, true);
    radio_unlock();
    if (held) {
        split_held_look();  /* the dial no longer keys: its hold goes before the app keys */
    }
}

uint16_t radio_change_vol(int16_t df) {
    int32_t vol = param_i_get(cfg_volume);
    if (df == 0) {
        return vol;
    }

    mute = false;
    if (param_i_get(cfg_speaker_off) && !btaudio_speaker_muted()) {
        msg_update_text_fmt("Speaker always off: Settings > Audio");   /* VOL still sets the value */
    }

    uint16_t new_val = limit(vol + df, 0, BOARD_VOL_MAX);

    if (new_val != vol) {
        param_i_set(cfg_volume, new_val);
    };

    return new_val;
}

void radio_change_mute() {
    mute = !mute;
    radio_vol_refresh();
}

/* Speaker AF and TX monitor together: both 0 when muted, with Speaker always off (a remote station), when btaudio
 * silences the speaker (headphones connected) or while a TCI client transmits; the monitor 0 at AF 0 too, which otherwise still played FT8's
 * tones (issue #1). A screen recording keeps the monitor at 30 at least on the X6100, but not under a TCI client's PTT */
void radio_vol_refresh() {
    bool    muted = mute || tci_mute || scan_mute || btaudio_speaker_muted() || param_i_get(cfg_speaker_off);
    int32_t vol   = param_i_get(cfg_volume);
    int32_t moni  = param_i_get(cfg_moni);
#ifdef BOARD_X6200
    /* A screen recording leaves the monitor alone here: the X6200's speaker shares the case with the
     * built-in microphone, and a monitor at 30 rang back into the transmission (issue #15) */
    bool    rec_raise = false;
#else
    bool    rec_raise = rec_moni && !tci_mute;
#endif

    if (rec_raise) moni = LV_MAX(moni, 30);
    else if (muted || vol == 0) moni = 0;
    radio_lock();
    hw_set_i(HW_RXVOL, muted ? 0 : vol);
    hw_set_i(HW_MONI, moni);
    radio_unlock();
}

/* A TCI client keyed the radio: the operator is at the other end, so the speaker and the TX
 * monitor stay quiet here until it unkeys; then both come back as they were */
void radio_set_tci_mute(bool on) {
    if (tci_mute == on) return;
    tci_mute = on;
    radio_vol_refresh();
}

void radio_set_scan_mute(bool on) {
    if (scan_mute == on) return;
    scan_mute = on;
    radio_vol_refresh();
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
    float xvtr;

    if (tx_refused(TX_BY_GUI)) {
        return;
    }
    /* In split the dial too (#121): the X6200's tuner went out on VFO A with A in use and B the TX VFO (#201, base
     * 1.0.8); which one the X6100's keys is not known */
    if (param_i_get(cfg_band_split) && tx_refused_at(cparam_i_get(cfg_fg_freq), TX_BY_GUI)) {
        return;
    }
    /* The tuner's carrier is the base's own, at a power the GUI does not set: not into a transverter with a TX power
     * limit (#104). What it would tune there is the IF into the transverter. Short: the box scrolls a longer line */
    WITH_RADIO_LOCK(xvtr = xvtr_cap);
    if (xvtr > 0.0f) {
        msg_schedule_text_fmt("No tuner: %.1f W transverter", xvtr);
        return;
    }
    WITH_RADIO_LOCK(open_tx_key());     /* the X6200's open TX still due first: the tuner keys at the next packet */
    atomic_store(&atu_band_stop, false);    /* one asked of a tune over already */
    state_move(RADIO_RX, RADIO_ATU_START);
}

/* The X6200's base (1.0.8) transmits in its scan mode but reports SWR 1.0 there, the stock firmware's scan too;
 * a plain carrier reads it. Keyed as the digital modes are (not radio_set_modem: no app is on air), so the mics
 * stay out, their gain 0 still lets a voice through; nothing plays to the base meanwhile. In FM: an AM carrier
 * at 1 W is ~0.2 W, too little for the bridge */
#ifdef BOARD_X6200
#define SWRSCAN_TX      HW_MODEM
#define SWRSCAN_MODE    radio_mode_nfm
#define SWRSCAN_MIN_MS  1200    /* from a move: the power comes up in about that */
#define SWRSCAN_FLAT_MS 300     /* without the power going up: it is up */
#define SWRSCAN_WAIT_MS 3000    /* a power that never settles is read anyway */

/* In ms, not packets: the X6200's come every ~35 ms (hw_x6200.c). The radio thread's own (radio_tick), but for
 * swrscan_fresh, which radio_start_swrscan sets for it */
static atomic_bool swrscan_fresh;       /* a new scan: the wait starts afresh */
static uint8_t     swrscan_peak;        /* the power's top since the move */
static uint64_t    swrscan_move_ms;     /* the move, or the key */
static uint64_t    swrscan_up_ms;       /* the power last went up */
#else
#define SWRSCAN_TX      HW_SWRSCAN
#define SWRSCAN_MODE    radio_mode_am
#endif

/* Nothing moves before the scan is known to start: the VFO that transmits goes to `from` only then */
bool radio_start_swrscan(int32_t from) {
    if (tx_refused(TX_BY_GUI) || !state_move(RADIO_RX, RADIO_SWRSCAN)) {
        return false;
    }

    radio_set_tx_freq(from);
    cparam_i_set(cfg_cur_mode, SWRSCAN_MODE);
    radio_lock();
    /* 1 W, held there by set_txpwr (SWRSCAN_PWR_W); less if PWR is set lower, or a transverter's limit.
     * ponytail: fixed 1 W cap, make it a setting if the bridge reads poorly that low */
    set_txpwr(param_f_get(cfg_pwr));
#ifdef BOARD_X6200
    atomic_store(&swrscan_fresh, true);
#endif
    open_tx_key();      /* the X6200's open TX still due goes first, on `from` */
    hw_set_i(SWRSCAN_TX, true);
    radio_unlock();

    return true;
}

void radio_stop_swrscan() {
    if (state_move(RADIO_SWRSCAN, RADIO_RX)) {
        radio_lock();
        hw_set_i(SWRSCAN_TX, false);
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

bool radio_digi_cap(void) {
    return digi_cap;
}

void radio_set_tune(float w, uint32_t limit_ms) {
    pthread_mutex_lock(&tune_mux);
    tune_guard = (tune_guard_t){.pwr_w = w, .start_ms = get_time(), .limit_ms = limit_ms};
    pthread_mutex_unlock(&tune_mux);
    radio_lock();
    tune_cap = w;
    set_txpwr(param_f_get(cfg_pwr));    /* tx_player sets the tune's own: this is the cap, and the setting after */
    radio_unlock();
}

#ifdef BOARD_PWR_MAX_BAT
/* The radio went onto its battery, its top down to `top`: a tune on the air above that is held to it now. The tune's
 * own power (up to 8 W on external power, #87) went to the base as it keyed, and with PWR at the battery's top or
 * under it nothing set the power again: the tune went on at 8 W on the battery, up to its 30 s */
static void tune_cap_top(float top) {
    radio_lock();
    if (tune_cap > top) {
        tune_cap = top;
        set_txpwr(txpwr_asked);
    }
    radio_unlock();
}
#endif

/* The VFO that transmits is the active one: tx_player moves it to the TX frequency, split held off (radio_set_tx_freq) */
void radio_set_tx_mode(int mode) {
    radio_lock();
    atomic_store(&tx_mode, mode);
    if (mode < 0) {
        hw_vfo_set_mode(RADIO_VFO_A, param_i_get(cfg_band_vfoa_mode));
        hw_vfo_set_mode(RADIO_VFO_B, param_i_get(cfg_band_vfob_mode));
    } else {
        hw_vfo_set_mode(param_i_get(cfg_band_current_vfo), mode);
    }
    radio_unlock();
}

int radio_tx_mode(void) {
    return atomic_load(&tx_mode);
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

/* This run's log onto the card: x6100_daemon moves /tmp/x6100_log.txt to app_logs only when the GUI exits, and
 * a power-off never lets it, so the log of the run a problem happened in was lost (and asked for in reports) */
static void save_log(void) {
    char       path[128];
    time_t     now = time(NULL);
    struct tm  tm;
    FILE      *in, *out;
    char       buf[4096];
    size_t     n;

    fflush(stdout);
    fflush(stderr);
    localtime_r(&now, &tm);
    snprintf(path, sizeof(path), BOARD_DATA "/app_logs/x6100_log_%04d-%02d-%02d_%02d-%02d-%02d_off.txt", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (!(in = fopen("/tmp/x6100_log.txt", "r"))) return;
    if ((out = fopen(path, "w"))) {
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0 && fwrite(buf, 1, n, out) == n) {}
        fflush(out);
        fsync(fileno(out));
        fclose(out);
    }
    fclose(in);
}

void radio_poweroff() {
    save_log();
    screen_rec_shutdown();
    settings_window_poweroff();     /* a Measure under way: PRE, ATT and the RF gain back before they are saved */
    if (params.charger.x == RADIO_CHARGER_SHADOW) {
        WITH_RADIO_LOCK(hw_set_i(HW_CHARGER, true));
    }
    cfg_api_flush_all();
    cfg_db_shutdown();
    /* Onto the card before the base cuts the power: the settings are written with synchronous=OFF (params/db.c), the
     * FAT keeps its table and folder changes in memory, and the kernel writes them out some 30 s later. Cut before,
     * params.db and its WAL can be left half written on the card, and the files written last lost */
    sync();
    state = RADIO_POWEROFF;
}

void radio_set_charger(bool on) {
    charger_asked = on;
    WITH_RADIO_LOCK(hw_set_i(HW_CHARGER, on));
    if (!charge_flag_counts() && pack_vext >= 30) {
        clock_update_estimate(-1, false);   /* no time to full in the top bar right away */
    }
}

/* KEY Train practice in the CW Trainer (#121): its key sounds the sidetone, at 0 W (train_hold) */
static bool practice(void) {
    return atomic_load(&train_hold) && cw_trainer_on();
}

/* Keying from the GUI where the radio may not transmit is refused, with a word on the screen (cfg_tx_refusal): where
 * the hardware does not (the X6200's FM broadcast and airband, what the WR12 receives, above HF without a
 * transverter: "receive only"), and on HF outside the amateur bands with TX outside them off (#121). Where it
 * transmits: radio_tx_freq, the other VFO in split, wherever a TX shift put it. PTT, the modem (the apps), CW, the
 * tuner and the SWR scan come through here; what the base keys by itself (its hand-mic PTT, VOX, the KEY jack) is
 * not, it is held at 0 W there instead (band_hold). The CW Trainer's key goes through outside the bands, where the
 * hardware transmits, its PTT is refused without a word: the key that follows it sounds. Not while the X6200's WR12
 * is out of its bypass, or may be (cfg_tx_block, #134): then nothing goes, anywhere */
static bool tx_refused_at(int32_t freq, tx_by_t by) {
    static uint64_t said;

    if (!cfg_tx_refusal(freq)) {
        return false;
    }
    if (by != TX_BY_GUI && practice() && cfg_is_tx_freq(freq) && !cfg_tx_block()) {
        return by == TX_BY_PTT;
    }
    if (get_time() - said > 5000) {     /* once, not for every CW element nor each slot of a beacon */
        said = get_time();
        radio_say_no_tx("No TX", freq, false);
    }
    return true;
}

bool radio_say_no_tx(const char *what, int32_t freq, bool now) {
    char        mhz[24];
    const char *block = cfg_tx_block();

    if (!cfg_tx_refusal(freq)) {
        return false;
    }
    cfg_tx_mhz(freq, mhz, sizeof(mhz));
    if (now) {
        msg_update_text_fmt("%s on %s", what, mhz);
    } else {
        msg_schedule_text_fmt("%s on %s", what, mhz);
    }
    /* Where the hardware transmits it is the bands (cfg_tx_refusal): where the switch is. The WR12 holds it anywhere */
    if (block) {
        msg_schedule_text_fmt("%s: no TX", block);
    } else {
        msg_schedule_text_fmt("%s", cfg_is_tx_freq(freq) ? "Out of band: Settings > TX Cal" : "Receive only there");
    }
    return true;
}

bool radio_no_data(void) {
    uint64_t pack = atomic_load(&data_time);    /* before the clock: never a packet newer than now */

    return base_no_data(get_time(), pack, pack != 0);
}

bool radio_say_no_data(const char *what, bool now) {
    if (!radio_no_data()) {
        return false;
    }
    if (now) {
        msg_update_text_fmt("%s: no data from the base", what);
    } else {
        msg_schedule_text_fmt("%s: no data from the base", what);
    }
    msg_schedule_text_fmt("Baseband: Settings > Info");     /* the notice's pointer (base_gone) */
    return true;
}

/* While the base sends no data (#152, radio_no_data) what the GUI keys is refused, said as tx_refused_at says its own:
 * the radio would transmit with no SWR protection, no power reading and no TX on the screen. A normal start's first
 * packet can take 16 s (#47): a key before it is refused too, one pressed again after it goes. What the base keys by
 * itself (the hand mic's PTT, VOX, the KEY jack) is not the GUI's to refuse. The CW Trainer's key still sounds at 0 W,
 * its PTT quietly not, as outside the bands. A TX under way as the data stops: the flow resets end it (radio_tick) */
static bool no_data_refused(tx_by_t by) {
    static uint64_t said;

    if (!radio_no_data()) {
        return false;
    }
    if (by != TX_BY_GUI && practice()) {
        return by == TX_BY_PTT;
    }
    if (get_time() - said > 5000) {     /* once, not for every CW element nor each slot of a beacon */
        said = get_time();
        radio_say_no_data("No TX", false);
    }
    return true;
}

/* The modem goes out on radio_tx_freq (radio_set_tx_freq moves the VFO in use there), and on the X6200 so does PTT
 * (ptt_moves); the GUI's CW key in split on the dial too (split_keys_dial), and PTT with split held off and the dial
 * back. Where it may, not without data from the base either */
static bool tx_refused(tx_by_t by) {
    return tx_refused_at(radio_tx_freq(), by) ||
           (by != TX_BY_GUI && !(by == TX_BY_PTT && ptt_moves()) && split_keys_dial() &&
            tx_refused_at(vfo_freq(true), by)) ||
           no_data_refused(by);
}

static atomic_bool ptt_on;      /* the GUI's PTT as last set: the keyboard's toggle follows it (kbd_ptt.h) */
static atomic_bool modem_on;    /* read on other threads (Winlink's on_air) while tx_player's thread keys */
static bool modem_moved;        /* the VFO in use on the TX frequency for the modem: back when it lets go. key_mux */
static pthread_mutex_t key_mux = PTHREAD_MUTEX_INITIALIZER;     /* PTT from CAT's thread and the GUI's, the modem */

/* The X6200's base keys the VFO in use whatever split says, on Xiegu's firmware too (#201: PTT over CAT measured on VFO
 * A with the screen on B). So on the TX frequency for the key, as the modem goes (radio_set_modem), with the mode of
 * the VFO in use. The hand mic, VOX and the KEY jack the base keys by itself: those stay on the dial */
bool radio_set_ptt(bool tx) {
    if (tx && tx_refused(TX_BY_PTT)) {
        return false;
    }
    pthread_mutex_lock(&key_mux);
    if (tx && ptt_moves()) {
        atomic_store(&ptt_moved, true);
        if (!atomic_load(&cur_on_tx)) {     /* else an app or the modem has it there, PTT keeps it there from now on */
            radio_set_tx_freq(radio_tx_freq());
            split_held_look();  /* held already and the base back on the dial: this look lifts its hold */
        }
        on_train_hold_change(NULL, NULL);   /* the other VFO's CW holds under PTT, also when nothing moved */
    }
    radio_lock();
    if (tx) {
        open_tx_key();  /* the X6200's open TX still due goes first */
    }
    hw_set_i(HW_PTT, tx);
    radio_unlock();
    atomic_store(&ptt_on, tx);
    if (!tx && atomic_exchange(&ptt_moved, false)) {
        /* The audio goes on, its end takes the VFO back; the SWR scan's sweep goes on, its window does. Its carrier is
         * keyed without modem_on (radio_start_swrscan) */
        if (atomic_load(&modem_on)) {
            modem_moved = true;
            on_train_hold_change(NULL, NULL);
        } else if (atomic_load(&state) == RADIO_SWRSCAN) {
            on_train_hold_change(NULL, NULL);
        } else {
            radio_set_tx_freq(0);
        }
    }
    pthread_mutex_unlock(&key_mux);
    return true;
}

bool radio_get_ptt(void) {
    return atomic_load(&ptt_on);
}

static int mic_override = -1;      /* the walkie screen's mic, cfg_mic stays as saved */

void radio_set_mic_override(int mic) {
    mic_override = mic;
    WITH_RADIO_LOCK(hw_set_i(HW_MIC, mic < 0 ? param_i_get(cfg_mic) : mic));
}

int radio_get_mic(void) {
    return mic_override < 0 ? param_i_get(cfg_mic) : mic_override;
}

/* In split the audio goes out on the TX frequency through radio_set_tx_freq, as the apps' tone does (TCI's audio,
 * the voice messages): the active VFO moved before keying and back after; an app that moved it for its tone keeps
 * its own */
void radio_set_modem(bool tx) {
    if (tx && tx_refused(TX_BY_GUI)) {
        return;
    }
    pthread_mutex_lock(&key_mux);
    if (tx && param_i_get(cfg_band_split) && !atomic_load(&split_held)) {
        radio_set_tx_freq(radio_tx_freq());
        modem_moved = true;
    }
    if (tx) {
        WITH_RADIO_LOCK(open_tx_key());     /* the X6200's open TX still due goes first, on the TX frequency */
    }
    atomic_store(&modem_on, tx);
    WITH_RADIO_LOCK(hw_set_i(HW_MODEM, tx));
    if (!tx && modem_moved) {
        modem_moved = false;
        radio_set_tx_freq(0);
    }
    pthread_mutex_unlock(&key_mux);
}

bool radio_get_modem(void) {
    return atomic_load(&modem_on);
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

int radio_cw_tone_dir(radio_mode_t mode) {
    if (mode == radio_mode_cwr || (mode == radio_mode_cw && (radio_caps() & HW_CAP_CW_TONE_RISES))) {
        return 1;
    }
    return mode == radio_mode_cw ? -1 : 0;
}

const char *radio_base_ver_str(void) {
    return hw_base_ver_str();
}

#ifdef BOARD_X6200
static pa_bias_t pa_bias;

/* The PA bias of this radio's base, from its calibration block as it was before this run (pa_bias.c, #173), to the
 * base and to the base log. Once: the library's copy of register 41 keeps it, and the keepalive and every resend of
 * the register block (hw_resend_all) send that copy */
static void pa_bias_send(void) {
    size_t         len;
    const uint8_t *block = hw_cal_block(&len);
    char           line[160];

    pa_bias = pa_bias_pick(block, len);
    pa_bias_log_line(&pa_bias, line, sizeof(line));
    base_guard_log("PA %s", line);
    hw_set_i(HW_BIAS_DRIVE, pa_bias.drive);
    hw_set_i(HW_BIAS_FINAL, pa_bias.final);
}

void radio_pa_bias_info(char *buf, size_t size) {
    pa_bias_info(&pa_bias, buf, size);
}
#endif

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
    rec_moni = on;
    radio_vol_refresh();
}

bool radio_set_morse_key(bool on) {
    if (on && tx_refused(TX_BY_MORSE)) {
        return false;
    }
    if (on && !morse_on) {
        open_tx_cw_down();  /* the X6200's open TX still due goes first */
    }
    morse_on = on;
    morse_ms = get_time();
    if (on) {
        open_tx_cw_wait();  /* a pin, not under the lock: not in the middle of a write to the base */
    }
    hw_pin_set(HW_PIN_MORSE_KEY, on);
    atomic_store(&morse_down_ms, on ? morse_ms | 1 : 0);
    return true;
}

void radio_set_morse_key_limit(uint32_t ms) {
    atomic_store(&morse_limit_ms, ms);
}

unsigned radio_morse_key_cuts(void) {
    return atomic_load(&morse_cuts);
}

/* The TX time limit of a key-down (QRSS3's, cw_encoder.c): the GUI's key down past it is let go here, on the radio
 * thread, whatever holds it; the keyer sees the count move and stops */
static void morse_limit_check(void) {
    uint32_t limit = atomic_load(&morse_limit_ms);
    uint32_t down  = atomic_load(&morse_down_ms);

    if (key_limit_over((uint32_t)now_time, down, limit)) {
        radio_set_morse_key(false);
        atomic_fetch_add(&morse_cuts, 1);
        fprintf(stderr, "radio: the CW key down over %u ms, let go\n", (unsigned)limit);
    }
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

/* RIT to the base. On the X6200's WR12's IF the base takes I and Q the other way round (wr12_follow), and that mirrors
 * its offset: at 2 m RIT +500 Hz took a carrier's tone 500 Hz up where on HF it goes 500 Hz down (#134, ea4iln's
 * measurement). So the other way round there. Under the lock */
static bool rit_mirror;

static void rit_send(void) {
    int32_t rit = param_i_get(cfg_rit);

    hw_set_i(HW_RIT, rit_mirror ? -rit : rit);
}

static void on_rit_change(Subject *subj, void *user_data) {
    WITH_RADIO_LOCK(rit_send());
}

static void on_change_f(Subject *subj, void *user_data) {
    float v = subject_f_get((SubjectFloat*)subj);
    WITH_RADIO_LOCK(hw_set_f((hw_param_t)(intptr_t)user_data, v));
}

static void on_pwr_change(Subject *subj, void *user_data) {
    WITH_RADIO_LOCK(set_txpwr(subject_f_get((SubjectFloat*)subj)));
}

static bool is_cw_mode(int32_t mode) {
    return mode == radio_mode_cw || mode == radio_mode_cwr;
}

/* KEY Train on, and the VFO in use in CW or CW-R, or in split the other one, which transmits: the power at the base
 * held at 0 W (set_txpwr, #127), in every key mode. Out of CW the power comes back as asked: the apps and the voice
 * modes are not the keyer's, and a digital app's TX level would climb at 0 W. While the base's split bit is held off
 * (radio_set_tx_freq: an app, TCI's audio, a voice message, the SWR scan) the VFO in use transmits, in its own mode:
 * the other one's CW held an FT8 slot at 0 W. Under the lock: radio_set_tx_freq calls it from the apps' threads */
static void on_train_hold_change(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    radio_lock();

    bool    a     = param_i_get(cfg_band_current_vfo) == RADIO_VFO_A;
    int32_t mode  = param_i_get(a ? cfg_band_vfoa_mode : cfg_band_vfob_mode);
    int32_t other = param_i_get(a ? cfg_band_vfob_mode : cfg_band_vfoa_mode);
    /* Held off for an app's slot the other VFO's CW holds nothing; under PTT it does, whichever way it got moved */
    bool    split = param_i_get(cfg_band_split) && (!atomic_load(&split_held) || atomic_load(&ptt_moved));
    bool    hold  = param_i_get(cfg_key_train) && (is_cw_mode(mode) || (split && is_cw_mode(other)));

    if (hold != atomic_load(&train_hold)) {
        atomic_store(&train_hold, hold);
        if (hold) {
            atomic_store(&train_said, false);
        }
        set_txpwr(txpwr_asked);
    }
    radio_unlock();
}

/* The base went into TX while KEY Train holds the power: the screen says why nothing goes out, once while it holds.
 * Not in the CW trainer, which is Train's own, nor for a TUNE, which keeps its power. Radio thread */
static void train_note(void) {
    bool tuning;

    if (!atomic_load(&train_hold) || cw_trainer_on()) {
        return;
    }
    pthread_mutex_lock(&tune_mux);
    tuning = tune_guard.pwr_w > 0.0f;
    pthread_mutex_unlock(&tune_mux);
    if (!tuning && !atomic_exchange(&train_said, true)) {
        msg_schedule_text_fmt("KEY Train: sidetone only, 0 W");
    }
}

/* Under the lock: the hold outside the amateur bands on or off (band_hold). The base gets the power, VOX and Train
 * at once; the user's VOX and Train settings stay as saved (on_vox_change, on_key_train_change). On the X6200 its
 * open TX looked at first, whether the hold changes or not (open_tx_look) */
static void band_hold_set(bool hold) {
    int32_t f;

    open_tx_look();
    if (hold == atomic_load(&band_hold)) {
        return;
    }
    atomic_store(&band_hold, hold);
    if (hold) {
        atomic_store(&band_said, false);
    }
    set_txpwr(txpwr_asked);
    hw_set_i(HW_VOX, param_i_get(cfg_vox_on) && !hold);
    hw_set_i(HW_KEY_TRAIN, param_i_get(cfg_key_train) || hold);
    radio_key_refused(&f);  /* the one refused: the dial where the base keys it (split_keys_dial) */
    LV_LOG_USER("Radio TX %s: TX at %i%s", hold ? "held at 0 W, VOX off, KEY Train on" : "hold off", f,
                atomic_load(&off_dial) ? ", Birdie Cal off the dial" : "");
}

/* VOX and KEY Train to the base as set, VOX off and Train on while band_hold holds. Under the lock, which
 * band_hold_set holds: a hold between the read and the write left VOX on at the base */
static void on_vox_change(Subject *subj, void *user_data) {
    (void)user_data;
    radio_lock();
    hw_set_i(HW_VOX, subject_i_get((SubjectInt*)subj) && !atomic_load(&band_hold));
    radio_unlock();
}

static void on_key_train_change(Subject *subj, void *user_data) {
    (void)user_data;
    radio_lock();
    hw_set_i(HW_KEY_TRAIN, subject_i_get((SubjectInt*)subj) || atomic_load(&band_hold));
    radio_unlock();
}

/* Under the lock, before a VFO of the base goes to freq: held at once where the radio would transmit there and may
 * not (#121), as xvtr_hold does for a transverter's limit (#104): a band key, a memory, a spot or CAT moves the VFOs
 * before the dial's frequency is notified, and a hand-mic PTT held through it sent PWR there meanwhile. The VFO that
 * transmits counts: the one in use, in split the other one; with a TX shift also the one in use at freq + or - the
 * shift (the other VFO follows it, tx_shift_follow, inside its round). The VFO in use in split only where the base
 * keys it (split_keys_dial: the X6200, split held off): elsewhere split going off and the VFO in use changing come
 * to band_look before the base. Holding too early is harmless, the hold goes only after the round (band_settle_cb).
 * True: held here */
static bool band_hold_early(radio_vfo_t vfo, int32_t freq) {
    bool    cur   = vfo == param_i_get(cfg_band_current_vfo);
    bool    split = param_i_get(cfg_band_split);
    int32_t shift = param_i_get(cfg_band_tx_shift);
    bool    keys  = cur ? !split || split_keys_dial() : split;

    if (!band_looked || atomic_load(&band_hold)) {
        return false;   /* not looked at yet (radio_start), or held already */
    }
    if ((keys && cfg_tx_refusal(freq)) ||
        (cur && split && shift && cfg_tx_refusal(vfo == RADIO_VFO_A ? freq + shift : freq - shift))) {
        band_hold_set(true);
        return true;
    }
    return false;
}

/* The GUI keys the radio: its PTT, the modem, the tuner, the SWR scan, the CW key (the CW Trainer's aside) */
static bool gui_keys(void) {
    radio_state_t s = atomic_load(&state);

    return s == RADIO_ATU_START || s == RADIO_ATU_WAIT || s == RADIO_ATU_RUN || s == RADIO_SWRSCAN ||
           hw_tx_keyed_by_gui() || atomic_load(&modem_on) || (morse_on && !practice());
}

/* The radio's tuner on, in split, with the dial where the radio may not transmit: which VFO its carrier goes out on
 * the GUI cannot tell (radio_start_atu), so the dial counts on both radios, where radio_key_refused counts it on the
 * X6200 only. Its carrier is the base's own, which the hold does not hold: not held, given up. *freq (may be NULL):
 * the dial */
static bool atu_dial_refused(int32_t *freq) {
    radio_state_t s = atomic_load(&state);
    int32_t       f = vfo_freq(true);

    if ((s != RADIO_ATU_START && s != RADIO_ATU_WAIT && s != RADIO_ATU_RUN) || !param_i_get(cfg_band_split) ||
        !cfg_tx_refusal(f)) {
        return false;
    }
    if (freq) {
        *freq = f;
    }
    return true;
}

/* The GUI's own TX on where the radio may not transmit, after a move, the switch or a band key while it keys (#121),
 * on the GUI thread: let go as swr_trip_cb does, the CW Trainer's key excepted. Held at 0 W meanwhile; it went on
 * there, an app's slot learning its level from 0 W. The SWR scan stops as its Stop does, the tuner gives up */
static void band_stop(int32_t freq) {
    radio_state_t s = atomic_load(&state);

    if (s == RADIO_SWRSCAN) {
        dialog_swrscan_stop(freq);
        radio_set_ptt(false);   /* a PTT under the scan (CAT, the PTT key) goes with it */
        return;
    }
    atomic_fetch_add(&band_stops, 1);   /* before the modem bit drops: tx_player keeps no level from it */
    if (s == RADIO_ATU_START || s == RADIO_ATU_WAIT || s == RADIO_ATU_RUN) {
        atomic_store(&atu_band_stop, true);
    }
    if (!practice()) {
        cw_encoder_stop();
        paddle_keyer_stop();
        radio_set_morse_key(false);
    }
    dialog_msg_voice_ptt_pressed();
    radio_set_modem(false);     /* the digital apps see it and stop (tx_player.c) */
    radio_set_ptt(false);
    radio_say_no_tx("TX stopped", freq, false);
}

/* Looked at again: where the radio may not transmit it holds at once; elsewhere the hold goes only after the round of
 * notifications (settled: band_settle_cb), once the base has the VFOs, split and the VFO in use that came with it, as
 * xvtr_pwr_look (at once if that job cannot be queued). The GUI's own TX still on there is stopped after the round */
static void band_look(bool settled) {
    bool    no, stop, hold, later;
    int32_t f;

    radio_lock();
    no    = radio_key_refused(&f);
    stop  = no || atu_dial_refused(&f);     /* the tuner in split: on the dial too, stopped without a hold */
    hold  = no || atomic_load(&off_dial);
    later = !settled && (stop ? gui_keys() : atomic_load(&band_hold));
    band_looked = true;
    if (hold || !later) {
        band_hold_set(hold);
    }
    radio_unlock();
    if (later && !scheduler_put_coalesced(band_settle_cb)) {
        WITH_RADIO_LOCK(band_hold_set(radio_key_refused(NULL) || atomic_load(&off_dial)));
    } else if (settled && stop && gui_keys() && scheduler_on_gui_thread()) {
        band_stop(f);
    }
}

static void band_settle_cb(void *arg) {
    (void)arg;
    band_look(true);
}

/* split_held came or went, or the VFO in use reached the TX frequency (radio_set_tx_freq): the base keys the dial
 * or no longer does (split_keys_dial), its hold with it, at once: an app's tone on a TX frequency in the bands is not
 * held at 0 W by a dial outside them. Any thread, no stop */
static void split_held_look(void) {
    radio_lock();
    if (band_looked) {
        band_hold_set(radio_key_refused(NULL) || atomic_load(&off_dial));
    }
    radio_unlock();
}

static void on_band_hold_change(Subject *subj, void *user_data) {
    (void)user_data;
    band_look(subj == NULL);
}

/* The base went into TX by itself while held (the hand mic's PTT, the KEY jack): the screen says why nothing goes out,
 * once while it holds. Not with KEY Train's own hold (train_note says it) nor in the CW Trainer. Radio thread */
static void band_note(void) {
    int32_t f;

    if (!atomic_load(&band_hold) || atomic_load(&train_hold) || cw_trainer_on() || atomic_exchange(&band_said, true)) {
        return;
    }
    if (radio_key_refused(&f)) {
        radio_say_no_tx("0 W", f, false);
    } else {
        msg_schedule_text_fmt("0 W during Birdie Cal");
    }
    fprintf(stderr, "radio: the base keyed by itself while held at 0 W (#121)\n");
}

unsigned radio_band_stops(void) {
    return atomic_load(&band_stops);
}

#ifdef BOARD_X6200
/* ---- The WR12 on the HOST port (wr12.c, #134) ---- */

/* The base listens on the WR12's IF for the VFO in use at freq: where the WR12 receives (WR12 on HOST, 54 MHz-1 GHz
 * that no transverter covers: cfg_wr12_freq), once it is set up, and not while Birdie Cal has the base off the dial */
static bool wr12_takes(int32_t freq) {
    return cfg_wr12_freq(freq) && wr12_ready() && !atomic_load(&off_dial);
}

/* Under the lock, the VFO in use just sent to the base: the WR12 follows the dial. Out of its bypass on it where the
 * base is on its IF (rx: held at 0 W already, by cfg_tx_refusal from wr12_want on), the base taking the IF's I and Q
 * the other way round; in its bypass elsewhere, IQ as it was. WR12 on HOST off: the WR12 let go */
static void wr12_follow(bool rx, int32_t dial) {
    hw_set_i(HW_IQ_SWAP, rx);
    if (rx != rit_mirror) {
        rit_mirror = rx;
        rit_send();
    }
    wr12_want(param_i_get(cfg_wr12), rx, dial);
}

/* Both VFOs to the base again, the WR12 following the one in use (on_vfo_freq_change), the one no longer in use back on
 * its own frequency. Not while Birdie Cal has the base off the dial: the WR12 in its bypass then, and the VFOs go back
 * at the run's end (radio_set_freq_dial). The WR12 tag in the frequency strip while it is in use (WR12 on HOST, set
 * up) */
static void wr12_retune(void) {
    if (atomic_load(&off_dial)) {
        WITH_RADIO_LOCK(wr12_follow(false, vfo_freq(true)));
    } else {
        on_vfo_freq_change(NULL, (void *)RADIO_VFO_A);
        on_vfo_freq_change(NULL, (void *)RADIO_VFO_B);
    }
    freq_strip_set_wr12(param_i_get(cfg_wr12) && wr12_ready());
}

/* On the GUI thread: what the WR12's thread has to say (found, lost, an ERROR answer); the base onto its IF now that it
 * is set up, or back on the dial, and the hold looked at again: TX goes free once the WR12 is seen back in its
 * bypass, or unplugged (cfg_tx_block) */
static void wr12_changed_cb(void *arg) {
    char note[64];

    (void)arg;
    while (wr12_note(note, sizeof(note))) {
        msg_schedule_text_fmt("%s", note);
    }
    wr12_retune();
    band_look(true);
}

/* From the WR12's thread: its news to the GUI thread. False: the queue is full, it asks again */
static bool wr12_notify(void) {
    return scheduler_put_coalesced(wr12_changed_cb);
}

/* The radio is in TX, or the GUI keys it (a PTT, the modem or the tuner's bit set before the base reports TX, the CW
 * Trainer's key too): the WR12 stays in its bypass meanwhile. Any thread */
static bool wr12_keyed(void) {
    return atomic_load(&state) != RADIO_RX || gui_keys() || morse_on;
}

/* Settings > General > WR12 on HOST (also the web Display page), or another VFO in use: the WR12 looked for or let
 * go, and following the dial */
static void on_wr12_change(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    wr12_retune();
}

/* ---- The base's open TX (HW_OPEN_TX, #176) ---- */

/* The base's own TX outside its band table (HW_OPEN_TX, byte 280 of its calibration block, found by ea4iln): at 1, as
 * x6200_control_init leaves it at every start, the base keys anywhere; at 0 only inside its own table (160 to 6 m,
 * 60 m 5.332-5.405 MHz), as Xiegu's app always has it. A second layer under the hold at 0 W, which alone let RF out:
 * 0.3 W from the hand mic on 2 m with the WR12 receiving, under 0.1 W on 5.060 MHz without it (an external meter on a
 * dummy load, base 1.0.8). Shut exactly while the GUI refuses TX for what the base keys by itself (radio_key_refused:
 * the WR12 on its IF, out of its bypass or asked out, receive only, outside the amateur bands with TX outside them
 * off, a split or a TX shift refused) and while Birdie Cal has the base off the dial; open otherwise. Our table is
 * wider than the base's (2200 m, 630 m, 5.1675 MHz, 60 m from 5.250 to 5.450 MHz, 8 m, a transverter's IF past
 * 29.7 MHz): shut there, PTT and the apps would key nothing. Open also where the CW Trainer's key goes through
 * outside the bands (tx_refused_at): base 1.0.8 sounds KEY Train's sidetone by going into TX at 0 W (#127), which
 * shut it may not do there, as its hand mic's PTT no longer did on 2 m (ea4iln; not tried with the trainer yet).
 * Onto the WR12's IF the shut goes at once, before the base goes there (band_hold_set from on_vfo_freq_change, and
 * band_look before the VFO in use changes), as soon as the base is in RX if it is not: not after the CW key's hang,
 * the GUI's CW does not key there and the WR12 leaves its bypass once the base is in RX (wr12_keyed), nor after the
 * wait for one that failed elsewhere. Anything else goes once the dial has settled, OPEN_TX_SETTLE_MS after the last
 * look, from the radio thread (open_tx_tick), the hold at 0 W covering the moment between: each write holds the radio
 * lock about 0.2 s, and one at each step of the knob over a band's edge stalled the knob. An open still due when the
 * GUI keys goes then, before the key (open_tx_key): else on a band of ours just tuned to, or an app's TX frequency
 * there in split, the base would send nothing for the whole TX. Never while the base is in TX, by itself too (the
 * hand mic's PTT at 0 W), nor while the GUI keys it or within its CW key's hang: what the base does with its working
 * calibration written in the middle of a TX is not known, so it waits for RX. The CW key is a pin, not under the
 * lock: it waits for a write on its way (open_tx_busy). One that failed goes again OPEN_TX_RETRY_MS later, from the
 * radio thread for a radio left alone too, twice as long after each one that fails again, a minute at most, and at
 * the next look once what is asked changes. A failure is said on the screen once each way, until one goes through.
 * open_tx_asked: what was asked last, -1 nothing yet; open_tx_base: what the base has, -1 not known: at the start
 * (the library's write is not looked at, so the first look reads it) and after one that failed. Under the lock;
 * open_tx_due, open_tx_now and open_tx_busy the other threads read without it */
static int8_t        open_tx_asked = -1;
static int8_t        open_tx_base = -1;
static atomic_ullong open_tx_due;       /* when the one asked goes to the base (get_time), 0 none */
static atomic_bool   open_tx_now;       /* that one the shut onto the WR12's IF: as soon as the base is in RX */
static atomic_bool   open_tx_busy;      /* a write on its way: the GUI's CW key waits for it (open_tx_cw_wait) */
static bool          open_tx_on_if;     /* the last look onto the WR12's IF */
static uint64_t      open_tx_retry_at;  /* not before this after one that failed, 0 none */
static uint32_t      open_tx_wait;      /* the wait after the last one that failed, 0 none */
static bool          open_tx_said[2];   /* a shut, an open that failed: said */

/* The base in TX or about to be: the radio's state (the base's own TX too), what the GUI keys, its CW key's hang */
static bool open_tx_keyed(void) {
    return wr12_keyed() || (uint32_t)get_time() - morse_ms < MORSE_HANG_MS;
}

/* What holds the one due back (open_tx_tick): the base in TX or the GUI keying it; for any but the shut onto the WR12's
 * IF also its CW key's hang, and the base log with no second free for its line (base_guard_log) */
static bool open_tx_held(void) {
    return atomic_load(&open_tx_now) ? wr12_keyed() : open_tx_keyed() || !base_guard_log_free();
}

/* What the base should have: 0 shut, 1 open (above) */
static int8_t open_tx_want(void) {
    int32_t f;

    if (atomic_load(&off_dial)) {
        return 0;
    }
    if (!radio_key_refused(&f)) {
        return 1;
    }
    return practice() && cfg_is_tx_freq(f) && !cfg_tx_block() && !wr12_takes(vfo_freq(true));
}

/* Under the lock: the one asked to the base. Not once the GUI's CW key is down: it looks at open_tx_busy after it set
 * morse_on, this at morse_on after it set open_tx_busy, so one of them sees the other (open_tx_cw_wait). False: not
 * sent, due again at once, held back by the key */
static bool open_tx_send(void) {
    int8_t want = open_tx_asked;
    bool   ok;

    atomic_store(&open_tx_busy, true);
    atomic_thread_fence(memory_order_seq_cst);
    if (morse_on) {
        atomic_store(&open_tx_busy, false);
        atomic_store(&open_tx_due, get_time());
        return false;
    }
    atomic_store(&open_tx_due, 0);
    ok = hw_set_i(HW_OPEN_TX, want);
    atomic_store(&open_tx_busy, false);
    if (ok) {
        open_tx_base = want;
        open_tx_wait = 0;
        open_tx_retry_at = 0;
        open_tx_said[0] = open_tx_said[1] = false;
        return true;
    }
    open_tx_base = -1;
    open_tx_wait = !open_tx_wait ? OPEN_TX_RETRY_MS
                 : open_tx_wait * 2 > OPEN_TX_RETRY_MAX_MS ? OPEN_TX_RETRY_MAX_MS : open_tx_wait * 2;
    open_tx_retry_at = get_time() + open_tx_wait;
    atomic_store(&open_tx_due, open_tx_retry_at);
    fprintf(stderr, "radio: the base's open TX not set to %d: tried again in %u s (#176)\n", want,
            (unsigned)(open_tx_wait / 1000));
    if (!open_tx_said[want]) {
        open_tx_said[want] = true;
        msg_schedule_text_fmt("%s", want ? "Base still keys only inside its own band table"
                                         : "Base's band table not set: TX held at 0 W only");
    }
    return true;
}

/* Under the lock, whenever the hold is looked at (band_hold_set): the one asked, sent at once onto the WR12's IF (as
 * soon as the base is in RX, if it is not), else once the dial has settled (open_tx_tick). A new one, or the dial onto
 * the IF, goes without the wait for one that failed before */
static void open_tx_look(void) {
    int8_t   want = open_tx_want();
    bool     wr12 = !want && wr12_takes(vfo_freq(true));
    uint64_t now = get_time(), due = wr12 ? now : now + OPEN_TX_SETTLE_MS;

    if (want != open_tx_asked || (wr12 && !open_tx_on_if)) {
        open_tx_asked = want;
        open_tx_wait = 0;
        open_tx_retry_at = 0;
    }
    open_tx_on_if = wr12;
    atomic_store(&open_tx_now, wr12);
    if (open_tx_asked == open_tx_base) {
        atomic_store(&open_tx_due, 0);
        return;
    }
    if (wr12 && now >= open_tx_retry_at && !wr12_keyed() && open_tx_send()) {
        return;
    }
    atomic_store(&open_tx_due, due > open_tx_retry_at ? due : open_tx_retry_at);
}

/* Under the lock, the GUI about to key the base (its PTT, the modem, the CW key, the tuner, the SWR scan): an open
 * still due goes now, while the base is in RX, not once the TX is over (above). The key comes about 0.2 s later then,
 * only then. Not after one that failed before its wait is over, nor with the base in TX by itself, something of the
 * GUI's keying it already or within its CW key's hang (the tuner's and the SWR scan's states are set before their
 * key) */
static void open_tx_key(void) {
    open_tx_look();
    if (open_tx_asked != 1 || open_tx_base == 1 || get_time() < open_tx_retry_at ||
        atomic_load(&state) == RADIO_TX || hw_tx_keyed_by_gui() || morse_on ||
        (uint32_t)get_time() - morse_ms < MORSE_HANG_MS) {
        return;
    }
    open_tx_send();
}

/* The GUI's CW key about to go down (radio_set_morse_key): an open due first, as open_tx_key; the lock only then */
static void open_tx_cw_down(void) {
    if (atomic_load(&open_tx_due) || atomic_load(&open_tx_busy)) {
        WITH_RADIO_LOCK(open_tx_key());
    }
}

/* morse_on just set: a write on its way (open_tx_send) is waited for before the key goes down, the lock its end */
static void open_tx_cw_wait(void) {
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load(&open_tx_busy)) {
        radio_lock();
        radio_unlock();
    }
}

/* The radio thread's: the one asked to the base once it is due and nothing holds it back (open_tx_held). The CW
 * Trainer opened or closed is looked at here: nothing tells radio.c */
static void open_tx_tick(void) {
    static bool practising;
    uint64_t    due;

    if (practice() != practising) {
        practising = !practising;
        WITH_RADIO_LOCK(open_tx_look());
    }
    due = atomic_load(&open_tx_due);
    if (!due || get_time() < due || open_tx_held()) {
        return;
    }
    radio_lock();
    if (atomic_load(&open_tx_due) == due && !open_tx_held()) {
        open_tx_send();     /* a look meanwhile put it off, or the GUI keyed */
    }
    radio_unlock();
}
#else
static bool wr12_takes(int32_t freq) {
    (void)freq;
    return false;
}

static void wr12_follow(bool rx, int32_t dial) {
    (void)rx;
    (void)dial;
}

static void open_tx_look(void) {
}

static void open_tx_key(void) {
}

static void open_tx_cw_down(void) {
}

static void open_tx_cw_wait(void) {
}

static void open_tx_tick(void) {
}
#endif

/* Under the lock, before a VFO of the base goes to freq: a transverter's TX power limit there, lower than the one in
 * force, goes to the base first (#104). A band key, a memory, a spot or CAT moves the VFOs before the dial's frequency
 * is notified (on_xvtr_pwr_change), and with PTT held the base sent PWR on the transverter's IF meanwhile. The VFO in
 * use counts, and in split the other one; the limit where the radio is tuned is looked at again after the round
 * (xvtr_settle_cb). True: held here */
static bool xvtr_hold(radio_vfo_t vfo, int32_t freq) {
    float w = cfg_transverter_pwr_for(freq);

    if (xvtr_cap < 0.0f || (vfo != param_i_get(cfg_band_current_vfo) && !param_i_get(cfg_band_split))) {
        return false;   /* not looked at yet (radio_start), or the other VFO out of split */
    }
    if (w <= 0.0f || (xvtr_cap > 0.0f && w >= xvtr_cap)) {
        return false;
    }
    xvtr_cap = w;
    set_txpwr(txpwr_asked);
    return true;
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
    bool cur = vfo == param_i_get(cfg_band_current_vfo);
    bool ptt = atomic_load(&ptt_moved);
    if (cur) {
        atomic_store(&cur_on_tx, ptt);      /* on the dial, not an app's TX frequency: it keys here (band_hold_early) */
    }
    /* The VFO in use where the X6200's WR12 receives (#134): the base on the WR12's IF, held at 0 W and its open TX
     * shut before it goes there (open_tx_look), whatever the frequency allows (54-55 MHz with TX outside the bands on);
     * the other VFO on its own. Under PTT (ptt_moved) the VFO in use stays on the TX frequency, the dial and the WR12
     * wait for its release: the hold and a transverter's limit look at where it goes, before it goes (in a band key's
     * round this VFO hears first, the other one is told after). Where it may not transmit it does not go: at 0 W alone
     * RF came out on 2 m (#176). It stays where it was until the look that change brings holds and stops the key */
    bool    wr12 = cur && !ptt && wr12_takes(new_val);
    int32_t base = wr12 ? WR12_IF_HZ : new_val - shift;
    int32_t look = new_val;
    bool    stay = false;
    if (cur && ptt) {
        look = radio_tx_freq();
        base = look - cfg_transverter_shift_for(look);
        stay = cfg_tx_refusal(look) != NULL;
    }
    if (wr12) {
        band_hold_set(true);
    }
    bool held = xvtr_hold(vfo, look);
    bool band = band_hold_early(vfo, new_val);
    bool changed = !stay && hw_vfo_set_freq(vfo, base);
    if (cur) {
        wr12_follow(wr12, new_val);
    }
    radio_unlock();
    if (held && !scheduler_put_coalesced(xvtr_settle_cb)) {
        LV_LOG_ERROR("Radio: the transverter's TX power limit not looked at again (the next change does)");
    }
    if (band && !scheduler_put_coalesced(band_settle_cb)) {
        LV_LOG_ERROR("Radio: the hold outside the amateur bands not looked at again (the next change does)");
    }
    if (changed) {
        LV_LOG_USER("Radio set vfo %i freq=%i (%i)", vfo, new_val, base);
    }
    if (ptt && !cur) {
        on_vfo_freq_change(NULL, (void *)(intptr_t)param_i_get(cfg_band_current_vfo));    /* the TX frequency moved */
    }
}

/* Split turned off under PTT: the VFO in use on what the radio transmits on now, the dial */
static void on_split_change(Subject *subj, void *user_data) {
    if (atomic_load(&ptt_moved)) {
        on_vfo_freq_change(NULL, (void *)(intptr_t)param_i_get(cfg_band_current_vfo));
    }
}

/* A transverter's range or shift changed (Settings > General > TRANSVERTERS): both VFOs again with the shift that
 * now applies. A shift set to 0 (off) left the base on the IF until the next retune, and the hand mic's PTT, which
 * the base keys by itself, went out there: 29.500 MHz on 10 m for a 2 m 145.500 */
static void on_transverter_change(Subject *subj, void *user_data) {
    on_vfo_freq_change(NULL, (void *)RADIO_VFO_A);
    on_vfo_freq_change(NULL, (void *)RADIO_VFO_B);
}

/* A transverter's TX power limit came or went (#104), on the LVGL thread: PWR and its top follow it, and the screen
 * says so, as for the X6200's battery. In place of what it said last, not queued after it: the limit stepped in
 * Settings on its band would stack a message for each step */
typedef struct {
    float   w;          /* the limit, 0 none */
    int32_t freq;       /* where it comes from: the dial, or the frequency the radio transmits on */
    bool    on;         /* tuned through a transverter, one without a limit too: the PWR from before only off it */
} xvtr_pwr_t;

static void xvtr_pwr_cb(void *arg) {
    xvtr_pwr_t *x = arg;
    char        name[24];

    switch (cfg_pwr_xvtr_set(x->w, x->on)) {
        case PWR_XVTR_HELD:
            cfg_transverter_name(x->freq, name, sizeof(name));
            msg_update_text_fmt("TX power %.1f W: transverter %s", param_f_get(cfg_pwr), name);
            break;
        case PWR_XVTR_RESTORED:
            msg_update_text_fmt("TX power back to %.1f W", param_f_get(cfg_pwr));
            break;
        default:
            break;
    }
}

/* The limit for where the radio is tuned: the dial's transverter, or the one it transmits through (split's other VFO,
 * a TX shift), the lower where both have one. The base goes to it at once, whatever asked for the power last (an app's
 * own, a TUNE's); PWR and the screen after it (xvtr_pwr_cb). The first look (radio_start, no subject) always goes
 * there, a PWR a transverter took down before a restart comes back if the radio starts off it; the base gets PWR
 * right after it, held to the limit. Off a limit onto a transverter without one (Off in Settings, 70 cm with none
 * after 2 m), the PWR from before waits until the radio is off the transverters too.
 * A lower limit goes to the base at once; one higher or none, and the PWR from before, after the round of
 * notifications (xvtr_settle_cb), once the base has left the band: the VFOs, the split bit and the VFO in use move
 * the base in no set order with this, and with PTT held through a band key the base sent 10 W on the IF meanwhile */
typedef enum {
    XVTR_FIRST,         /* radio_start */
    XVTR_NOTIFIED,      /* where the radio is tuned or a transverter's setting changed */
    XVTR_SETTLED,       /* after that round */
} xvtr_look_t;

static void xvtr_pwr_look(xvtr_look_t how) {
    static xvtr_pwr_t said = {-1.0f, 0, false};     /* passed on to PWR last: -1 nothing yet */
    int32_t           rx   = cparam_i_get(cfg_fg_freq), tx = radio_tx_freq();
    xvtr_pwr_t        x    = {cfg_transverter_pwr_cap(rx, tx), 0,
                              cfg_transverter_shift_for(rx) != 0 || cfg_transverter_shift_for(tx) != 0};
    float             cap;

    x.freq = cfg_transverter_pwr_for(rx) == x.w ? rx : tx;
    WITH_RADIO_LOCK(cap = xvtr_cap);
    if (how == XVTR_NOTIFIED && !(x.w > 0.0f && (cap <= 0.0f || x.w < cap)) && (x.w != cap || x.on != said.on) &&
        scheduler_put_coalesced(xvtr_settle_cb)) {
        return;
    }
    radio_lock();
    if (x.w != xvtr_cap) {
        xvtr_cap = x.w;
        if (how != XVTR_FIRST) set_txpwr(txpwr_asked);
    }
    radio_unlock();
    if (x.w == said.w && x.on == said.on) {
        return;
    }
    said = x;
    if (x.w > 0.0f) {
        LV_LOG_USER("Radio TX power at most %.1f W: a transverter at %i", x.w, x.freq);
    }
    if (scheduler_on_gui_thread()) {
        xvtr_pwr_cb(&x);
    } else if (!scheduler_put_urgent(xvtr_pwr_cb, &x, sizeof(x))) {
        LV_LOG_ERROR("Radio: the transverter's TX power limit not passed on to PWR (the base holds it)");
    }
}

static void xvtr_settle_cb(void *arg) {
    xvtr_pwr_look(XVTR_SETTLED);
}

static void on_xvtr_pwr_change(Subject *subj, void *user_data) {
    xvtr_pwr_look(subj ? XVTR_NOTIFIED : XVTR_FIRST);
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

#ifndef BOARD_X6200
/**
 * Restore "listening" of main board and USB soundcard after ATU
 */
static void recover_processing_audio_inputs() {
    usleep(10000);
    radio_vfo_t vfo = param_i_get(cfg_band_current_vfo);
    radio_lock();
    hw_vfo_set_mode(vfo, radio_mode_usb_dig);
    hw_set_f(HW_TXPWR_F, atomic_load(&band_hold) ? 0.0f : 0.1f);   /* the dial moved out during the tune (#121) */
    hw_set_i(HW_MODEM, true);
    usleep(50000);
    hw_set_i(HW_MODEM, false);
    set_txpwr(param_f_get(cfg_pwr));
    hw_vfo_set_mode(vfo, cparam_i_get(cfg_cur_mode));
    radio_unlock();
}
#endif

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

/* The model, and the log fed again, once at the base's first reading, whatever it says: the stretch on battery goes
 * on across a GUI restart (the model learns only what is newer than it); no model yet, it learns from the old log
 * too. During the start, as before the readings had to settle (bat_watch.h): 15 s into a running GUI the replay of
 * a full log would hold the radio thread, and with it the spectrum, the S-meter and the X6200's register keepalive */
static void battery_model_load(void) {
    static bool done;

    if (done) {
        return;
    }
    done = true;

    uint64_t t0 = get_time();

    if (!bat_est_load(BATTERY_MODEL)) {
        bat_est_replay(BATTERY_LOG ".1");
    }
    bat_est_replay(BATTERY_LOG);
    bat_est_save(BATTERY_MODEL);
    fprintf(stderr, "battery: model and log read at %llu ms, in %llu ms\n", (unsigned long long)(now_time - start_time),
            (unsigned long long)(get_time() - t0));
}

/* volts_ext: on external power the % shown is the voltage's, the base has lost count (#223) */
static void battery_log(const hw_pack_t *p, bool volts_ext) {
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
    last = t;
    if ((f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r"))) {
        if (fscanf(f, "%u", &mhz) == 1) {
            mhz /= 1000;
        }
        fclose(f);
    }
    bat_sample_t s = {
        /* the % shown on battery (bat_watch_cap: the base's can be far off, #49), else the base's: the time to
         * full is learned in the base's % while charging (bat_est.c), as the log keeps it */
        .t = t, .vbat = p->vbat * 0.1f, .cap = bat_pct >= 0 ? bat_pct : p->batcap, .vext = p->vext * 0.1f,
        .charging = p->charging,
        .tx_s = tx_s, .tx_w = tx_s > 0 ? tx_ws / tx_s : 0, .bl = backlight_level(),
        .wifi = wifi_get_status() != WIFI_OFF,
        .bt = access("/sys/class/bluetooth/hci0", F_OK) == 0,
        .hp = access("/tmp/btaudio-hp", F_OK) == 0,
    };
    bool chg, approx;

    tx_s = tx_ws = 0;
    bat_est_feed(&s);
    int min = bat_est_minutes(0, &chg, &approx);

    if (volts_ext || (s.vext >= 3.0f && !charge_flag_counts())) {
        /* The charger is off: no time to full. Nor from the base's % next to the voltage's: #223's full pack on
         * the supply, the base at 2 %, would have shown 98 % and 8 h to full */
        min = -1;
    }
    clock_update_estimate(min, approx);

    /* On battery at 10% or less: a warning every minute (the 30 s power-off comes at BAT_LOW, bat_watch.h) */
    if (s.vext < 3.0f && s.vbat >= 3.0f && s.cap <= 10) {
        int w[2] = { s.cap, min };

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

/* On the GUI thread: the trace syncs the card, which the flow must not wait for */
static void start_trace_cb(void *arg) {
    start_trace("%s", (const char *)arg);
}

/* The base's supply readings until they count, into the run's log: what it sends at power-on, for a
 * report (#17). A line for each change, then one when they count */
static void battery_startup_log(const bat_watch_t *w, const hw_pack_t *p) {
    static bool     done;
    static uint16_t shown = UINT16_MAX;     /* vbat, batcap */
    static uint8_t  shown_ext;

    if (done) {
        return;
    }
    if (w->counts) {
        char line[96];

        snprintf(line, sizeof(line), "battery: readings count from %llu ms: %.1f V %u%%, external %.1f V",
                 (unsigned long long)(now_time - start_time), p->vbat * 0.1f, p->batcap, p->vext * 0.1f);
        fprintf(stderr, "%s\n", line);
        scheduler_put(start_trace_cb, line, sizeof(line));  /* the card's write off this thread */
        done = true;
        return;
    }
    if (shown == (p->vbat << 8 | p->batcap) && shown_ext == p->vext) {
        return;
    }
    shown = p->vbat << 8 | p->batcap;
    shown_ext = p->vext;
    fprintf(stderr, "battery: start-up reading at %llu ms: %.1f V %u%%, external %.1f V (flag %u, charging %u)\n",
            (unsigned long long)(now_time - start_time), p->vbat * 0.1f, p->batcap, p->vext * 0.1f, p->vext_flag,
            p->charging);
}

static swr_guard_t swr_guard;
static atomic_uint swr_trips;
static atomic_ullong swr_trip_at;       /* the last one, get_time(); 0 none */
static atomic_bool swr_trip_queued;     /* a trip's job waits in the queue: one stops TX, more add nothing */

/* SWR protection, on the GUI thread: what the GUI keys lets go (a PTT held on the radio or the
 * mic, VOX and the radio's own keyer are the base's: they may key again) */
static void swr_trip_cb(void *arg) {
    atomic_store(&swr_trip_queued, false);  /* out of the queue: a trip from now on is posted again */
    atomic_store(&swr_trip_at, get_time()); /* before the count: whoever sees the trip can tell when */
    atomic_fetch_add(&swr_trips, 1);    /* before the modem bit drops: whoever sees it gone can tell why */
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

unsigned radio_swr_trips(void) {
    return atomic_load(&swr_trips);
}

uint64_t radio_swr_trip_at(void) {
    return atomic_load(&swr_trip_at);
}

/* A flow reset's line for the base log: now if this second has no line yet (base_guard_log_free), else from the
 * first tick that finds one free (radio_tick), the step it tells of acting at once. One line a second gets in, and
 * "base silent" (every 10 s) or "register block not sent" (every 200 ms while I2C fails too) take the second: a tune
 * or an SWR scan's carrier that waited for a free one stayed on for seconds. One kept, a newer one in its place.
 * Radio thread */
static char later_line[160];

static void later_flush(void) {
    if (later_line[0] && base_guard_log_free()) {
        base_guard_log("%s", later_line);
        later_line[0] = 0;
    }
}

static void log_later(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(later_line, sizeof(later_line), fmt, ap);
    va_end(ap);
    later_flush();
}

#ifndef BOARD_X6200
/* A tune given up for want of data: the X6100's 0.1 W key after it waits for the first packet back (radio_tick), as
 * nothing the GUI keys goes out without data (#152). Radio thread */
static bool recover_due;
#endif

/* Out of TX or a tune: the TX meters' readings back to 0 (tx_info_clear), then the GUI told. Here, in the radio thread
 * that stores them, nothing of the TX that ended comes after: they stay 0 all through RX, and each transmission's
 * start from 0, also for a reader that looks before the base's first TX packet (TCI as it keys, #53) */
static void tx_over(void) {
    tx_info_clear();
    if (notify_rx_tx) {
        notify_rx_tx(false);
    }
}

/* The base never keyed for the tuner, or ended a tune without its done flag (no match: more likely on 10 and 15 m
 * with a real antenna). The GUI stayed in the tuner's state for good with the tune bit set: no new tune, the SWR
 * scan refused, CAT and TCI reporting TX (#25). Now the tune bit off, back to RX, a word on the screen and a line in
 * the base log with what the base reported. Also a tune given up where the radio may not transmit (#121), and one
 * the base sends no data for (#152), its line with how long */
static void atu_give_up(radio_state_t from, const char *why) {
    bool flow = now_time - pack_time > FLOW_RESTART_TIMEOUT;    /* from the flow resets: no data for seconds */

    WITH_RADIO_LOCK(hw_set_i(HW_ATU_TUNE, false));
#ifndef BOARD_X6200
    if (from == RADIO_ATU_RUN && flow) {
        recover_due = true;
    } else if (from == RADIO_ATU_RUN) {
        recover_processing_audio_inputs();      /* as after a tune that found its network */
    }
#endif
    if (!state_move(from, RADIO_RX)) {
        return;     /* powering off meanwhile */
    }
    if (from == RADIO_ATU_RUN) {
        tx_over();
    }
    if (flow) {
        /* from the flow resets: the packet's fields are the last one's, seconds old (a TX that was) */
        log_later("ATU: %s for %u ms", why, (unsigned)(now_time - pack_time));
    } else {
        base_guard_log("ATU: %s (tx %d, atu_status %d, atu_params %08x)", why, pack->tx, pack->atu_status,
                       (unsigned)pack->atu_params);
    }
    msg_schedule_text_fmt("%s", why);
}

/* The notice's lines (base_gone), whole in every font (DejaVu, the widest: 558 px at most), each as long as the base's
 * own warnings */
static const char *const gone_lines[] = {
    "No data from the base",
    "No spectrum, no SWR protection",
    "Baseband: Settings > Info",
    "Base versions: see the README",    /* an X6200's below 1.0.7 is not the only case */
};

#define GONE_LINE_MS        4000    /* the notice's next line as the one before goes (msg_box's long message) */

typedef struct {
    uint8_t line;       /* in gone_lines */
    bool    never;      /* none since the start: a start stage too */
} gone_line_t;

/* On the GUI thread: a line of the notice */
static void base_gone_cb(void *arg) {
    gone_line_t *g = arg;

    msg_schedule_long_text_fmt("%s", gone_lines[g->line]);
    if (g->never) {
        start_trace("base: no data in %u s", BASE_GONE_START_MS / 1000);
    }
}

/* No packet from the base for BASE_GONE_MS in receive, looked at in the flow resets (#152). The spectrum, the
 * waterfall, the S-meter, the battery, the TX meters and the SWR protection stood still, the tuner and the X6200's
 * POWER key did nothing, and only the base log said so ("base silent"). An X6200 base older than 1.0.7 does that:
 * the library reads the packets as 1.0.7 sends them (README, The baseband). Counted from the start, the last packet
 * or the GUI's last TX (base_gone_due, tools/base_guard tests it): BASE_GONE_START_MS at a start (one X6200 on 1.0.8
 * sent its first packet 16 s in, #47), not while the GUI keys, and a GUI restart (an update, LOCK's base restart)
 * counts afresh. The TX state alone does not count as keyed: one the GUI keys nothing in is the base's own or one
 * whose PTT off did not get through (radio_tick), which stays TX, and it is said then too. Said once, again only
 * after packets came (radio_tick) and stopped again. The first line at once, each next one GONE_LINE_MS on from the
 * resets, so none comes once the data is back: all four queued at once stayed 16 s after it, and held up any other
 * message (the tuner's) as long. True: its line went to the base log */
static bool base_gone(void) {
    static uint64_t tx_time, line_time;
    static uint8_t  line;       /* the next line of the notice */
    radio_state_t   s = atomic_load(&state);
    bool            keyed = (s != RADIO_RX && s != RADIO_TX) || hw_tx_keyed_by_gui() || atomic_load(&modem_on) ||
                            morse_on;
    bool            due = base_gone_due(now_time, pack_time, base_seen, keyed, &tx_time, gone_said);
    gone_line_t     g = {.line = 0, .never = !base_seen};

    if (gone_said) {
        if (line < ARRAY_SIZE(gone_lines) && now_time - line_time >= GONE_LINE_MS) {
            g.line = line;
            g.never = false;
            if (scheduler_put(base_gone_cb, &g, sizeof(g))) {
                line++;
                line_time = now_time;
            }
        }
        return false;
    }
    if (!due || !base_guard_log_free()) {
        return false;   /* a line written in this second: the next reset tries again (radio_tick) */
    }
    if (!scheduler_put(base_gone_cb, &g, sizeof(g))) {
        return false;   /* the next reset tries again (the note above radio_tick) */
    }
    gone_said = true;
    line = 1;
    line_time = now_time;
    base_guard_log("no data from the base for %u ms%s: said on the screen", (unsigned)(now_time - pack_time),
                   g.never ? ", none since the start" : "");
    return true;
}

/* On the GUI thread: the SWR scan stopped for want of data (radio_tick), asked once until it has run. "SWR scan: no
 * data from the base" fits whole (DejaVu 38: 570 px) */
static atomic_bool swrscan_stop_asked;

static void swrscan_gone_cb(void *arg) {
    dialog_swrscan_give_up("SWR scan: no data from the base");
    atomic_store(&swrscan_stop_asked, false);
}

/* What the base reports by a change alone goes to the GUI thread once: the low battery, the POWER key, a hand mic
 * key, the tuner's network. Nothing posts it again, so a job dropped by a full queue was lost for good: a POWER
 * release lost let the hold timer turn a tap into a power-off, a low battery lost never started the 30 s
 * power-off, a network lost was not kept (#33's start: the DSP's redraws held all 64 places for seconds). Such a
 * job takes the urgent reserve, and the change counts as told only once its job is in: if even the reserve is
 * full, the next packet (for the battery, the next reading) tries again */
static uint32_t atu_network;        /* the tuner's network, for atu_done_cb */
static bool     atu_network_due;    /* found, its job not in the queue yet */

static bool radio_tick() {
    static bool power_was;      /* the X6200's POWER key as the GUI was told it (the packets, the flow resets) */

    if (now_time < prev_time) {
        prev_time = now_time;
    }
    later_flush();

    int32_t d = now_time - prev_time;

    if (hw_flow_read(pack)) {
        /* Its gap kept until a line is free: right after a "base silent" line it was dropped (one a second) */
        static uint32_t again_ms;

        if (gone_said) {
            again_ms = now_time - pack_time;
        }
        if (again_ms && base_guard_log_free()) {
            base_guard_log("data from the base again after %u ms", (unsigned)again_ms);
            again_ms = 0;
        }
#ifndef BOARD_X6200
        /* Before the other threads see the data and may key, which its end would cut. Not while the base transmits
         * by itself (the hand mic's PTT, its tune still on): dropped then */
        if (recover_due) {
            recover_due = false;
            if (!pack->tx && atomic_load(&state) == RADIO_RX) {
                recover_processing_audio_inputs();
            }
        }
#endif
        base_seen = true;
        gone_said = false;      /* said again if it stops again (base_gone) */
        prev_time = now_time;
        pack_time = now_time;
        atomic_store(&data_time, now_time);
        pack_vbat = pack->vbat;
        pack_vext = pack->vext;

        static uint8_t delay;

        if (delay++ > 10) {
            delay = 0;
            /* The base's first readings after power-on are not the battery's (bat_watch.h): the top bar,
             * the battery log and time left, its warnings and the power-off wait until they count */
            static bat_watch_t bat_watch;

            bat_watch_reading(&bat_watch, pack->vbat, pack->vext, pack->vext_flag, pack->charging, pack->bat_missing,
                              pack->tx, now_time);

            battery_startup_log(&bat_watch, pack);
            battery_model_load();
            /* The X6200's base (1.0.8) raises its charging flag whenever external power is in, even
             * with the charger off, when it does not charge (measured on the radio: the battery stays
             * flat). bat_watch above still takes the flag as a sign of external power; from here on
             * it counts only if charging was asked for, as in Xiegu's app: the top bar, the time to
             * full and the battery log (charge_flag_counts: the X6100's as its base gives it) */
            pack->charging = pack->charging && charge_flag_counts();
            clock_update_no_battery(pack->bat_missing);

            /* The % sags under the TX current and comes back slowly: the last RX reading holds
             * while transmitting and for 30 s after (also in the top bar, clock.c). Before the top bar
             * and the battery log take it: they took the % of the reading before, and at the first
             * reading on battery that was none, so the base's went in. On #49's radio (the base at 0 %
             * with a full pack) that gave "Battery 0%: connect power" at each power-on and a time left
             * from 0h00, climbing back over the first ten minutes. On external power too, where it is the
             * base's or the voltage's where the base has lost count (#223): a supply that does not hold the
             * battery up lets it sag as well. Held only on the supply it was taken on: the base's 2 % from a
             * supply, held on battery, would have said "connect power" */
            static uint64_t hold_until;
            static int8_t   held_cap = -1;
            static bool     held_ext;
            int8_t          cap = bat_watch.counts ? pack->batcap : -1;

            if (radio_get_state() == RADIO_TX) hold_until = now_time + 30000;
            if (now_time < hold_until && held_cap >= 0 && cap >= 0 && held_ext == bat_watch.ext) {
                cap = held_cap;
            } else {
                if (cap >= 0) {     /* on receive only */
                    cap = bat_watch.ext ? bat_watch_cap_ext(&bat_watch, pack->vbat, pack->batcap)
                                        : bat_watch_cap(&bat_watch, pack->vbat, pack->batcap);
                }
                held_cap = cap;
                held_ext = bat_watch.ext;
            }
            bat_pct = bat_watch.ext ? -1 : cap;   /* the % on the screen on battery (clock.c) */
            if (bat_watch.counts) {
                clock_update_power(pack->vext * 0.1f, pack->vbat * 0.1f, cap, pack->charging);
                battery_log(pack, bat_watch.ext && bat_watch.by_volts);
            }

            /* The base's warnings, once its readings count (bat_watch.h): shown when one comes on, and again
             * every 30 s while any lasts. told: those shown in the last 30 s. One that blinks (the DC input
             * sagging under the TX current) is not new at each blink: each message stays 4 s and they queue,
             * so a blink every other reading would have queued them faster than they go. Shown only once
             * its job is in (the note above radio_tick) */
            static uint8_t  told;
            static uint64_t told_time;
            uint8_t         w = bat_watch.counts
                                    ? pack->bat_hot | pack->pa_hot << 1 | pack->dcin_low << 2 | pack->bat_cold << 3
                                    : 0;
            bool            due = now_time - told_time >= 30000;

            if (w && ((w & ~told) || due) && scheduler_put(base_warn_cb, &w, sizeof(w))) {
                told      = due ? w : told | w;
                told_time = now_time;
            }

            /* low_power: what the GUI was told (the note above radio_tick) */
            if (bat_watch.low != low_power) {
                bool low = bat_watch.low;

                if (scheduler_put_urgent(low_power_ui_cb, &low, sizeof(low))) {
                    low_power = low;
                }
            }

#ifdef BOARD_PWR_MAX_BAT
            /* The TX power cap follows the supply once it has been steady for 5 s, as Xiegu's app
             * waits. Until the readings count we assume external power, the cap we start with */
            static bool     supply_ext = true, seen_ext = true;
            static uint64_t seen_time;

            if (bat_watch.counts && bat_watch.ext != seen_ext) {
                seen_ext  = bat_watch.ext;
                seen_time = now_time;
            }
            if (seen_ext != supply_ext && now_time - seen_time >= 5000) {
                bool ext = seen_ext;

                if (scheduler_put(supply_cb, &ext, sizeof(ext))) {
                    supply_ext = ext;
                }
            }
#endif
        }
        if (pack->psd) {
            dsp_psd(pack->psd, pack->n_psd, pack->psd_span, pack->tx, pack->smeter_dbm);
        } else {
            dsp_samples((cfloat *)pack->samples, pack->n_samples, pack->tx, pack->lo_freq, pack->vary_freq, pack->fft_dec);
        }

        /* The POWER key where the base reports it (the X6200): to the GUI thread as a key. power_was: what
         * the GUI was told (the note above radio_tick) */
        if (pack->power_key != power_was) {
            bool down = pack->power_key;

            if (scheduler_put_urgent(power_key_cb, &down, sizeof(down))) {
                power_was = down;
            }
        }
        if (atu_network_due && scheduler_put_urgent(atu_done_cb, &atu_network, sizeof(atu_network))) {
            atu_network_due = false;
        }

        switch (state) {
            case RADIO_RX:
                if (pack->tx && state_move(RADIO_RX, RADIO_TX)) {
                    tx_by_gui = false;  /* set below while the GUI keys it */
                    if (notify_rx_tx) {
                        notify_rx_tx(true);
                    }
                    train_note();
                    band_note();
                }
                break;

            case RADIO_TX:
                if (!pack->tx && state_move(RADIO_TX, RADIO_RX)) {
                    tx_over();
                } else {
                    // printf("%d, %d\n", pack->tx_power, pack->alc_level);
                    tx_info_update(pack->tx_power * 0.1f, pack->vswr * 0.1f, pack->alc_level * 0.1f);
                    float swr = pack->vswr * 0.1f;
                    /* A stop: the urgent reserve. Dropped even so, swr_guard trips again 300 ms on. One in the
                     * queue at a time: with the GUI thread stalled and TX still on, a trip every ~320 ms took
                     * the reserve in 25 s from those that do not try again (TCI's unkey, FT8's last post).
                     * Marked before the put, the job may run before the put returns. A TUNE for an external
                     * tuner shows a high SWR on purpose while the tuner matches: none for the readings of its
                     * own power within its time (tune_swr_limit), as for the ATU's (#84) */
                    float limit = param_i_get(cfg_swr_protect) * 0.1f;

                    pthread_mutex_lock(&tune_mux);
                    limit = tune_swr_limit(&tune_guard, pack->tx_power * 0.1f, limit, now_time);
                    pthread_mutex_unlock(&tune_mux);
                    if (swr_guard_check(&swr_guard, pack->tx_power * 0.1f, swr, limit, now_time) &&
                        !atomic_exchange(&swr_trip_queued, true) && !scheduler_put_urgent(swr_trip_cb, &swr, sizeof(swr))) {
                        atomic_store(&swr_trip_queued, false);
                    }
                }
                break;

            case RADIO_ATU_START:
                /* Moved where the radio may not transmit since the press, or the switch turned off (#121): its
                 * carrier is the base's own, which the hold's 0 W does not hold, so it never starts. In split either
                 * VFO, as radio_start_atu */
                if (atomic_exchange(&atu_band_stop, false) || atomic_load(&band_hold) || atu_dial_refused(NULL)) {
                    atu_give_up(RADIO_ATU_START, "Tuner stopped");
                    break;
                }
                WITH_RADIO_LOCK(hw_set_i(HW_ATU_TUNE, true));
                atu_time = now_time;
                state_move(RADIO_ATU_START, RADIO_ATU_WAIT);
                break;

            case RADIO_ATU_WAIT:
                if (atomic_exchange(&atu_band_stop, false)) {
                    atu_give_up(RADIO_ATU_WAIT, "Tuner stopped");   /* band_stop said why */
                } else if (pack->tx) {
                    if (notify_rx_tx) {
                        notify_rx_tx(true);
                    }
                    atu_time = now_time;
                    state_move(RADIO_ATU_WAIT, RADIO_ATU_RUN);
                } else if (now_time - atu_time > ATU_START_MS) {
                    atu_give_up(RADIO_ATU_WAIT, "Tuner did not start");
                }
                break;

            case RADIO_ATU_RUN:
                if (atomic_exchange(&atu_band_stop, false)) {
                    atu_give_up(RADIO_ATU_RUN, "Tuner stopped");
                } else if (pack->atu_status && !pack->tx) {
                    /* The settings on the LVGL thread: their observers run where they are set, and
                     * the delayed ones (the info bar) schedule LVGL calls, which only that thread may.
                     * Not in the queue: the next packets try again (the note above radio_tick) */
                    atu_network     = pack->atu_params;
                    atu_network_due = !scheduler_put_urgent(atu_done_cb, &atu_network, sizeof(atu_network));
                    WITH_RADIO_LOCK(hw_set_i(HW_ATU_TUNE, false));
#ifndef BOARD_X6200
                    /* The X6100's: it keys the modem at 0.1 W; gdyuldin's X6200 GUI dropped it */
                    recover_processing_audio_inputs();
#endif
                    tx_over();

                    // TODO: change with observer on atu->loaded change
                    WITH_RADIO_LOCK(hw_set_i(HW_ATU_NETWORK, pack->atu_params));
                    state_move(RADIO_ATU_RUN, RADIO_RX);
                } else if (pack->tx) {
                    tx_info_update(pack->tx_power * 0.1f, pack->vswr * 0.1f, pack->alc_level * 0.1f);
                    atu_time = now_time;
                } else if (now_time - atu_time > ATU_NO_MATCH_MS) {
                    /* out of TX without the done flag; the X6200 library's own example ends a tune there */
                    atu_give_up(RADIO_ATU_RUN, "Tuner: no match");
                }
                break;

            case RADIO_SWRSCAN:
#ifdef BOARD_X6200
                /* Not keyed yet, or the power still coming up: it starts from 0 at each move and comes up
                 * in 0.1 W steps over ~1 s, and the SWR reads low meanwhile (1.0-1.1 at 0.2 W where 0.8 W reads
                 * 1.5). Where it settles depends on the SWR (0.8 W at 1.5, 0.6 W at 3.4): read once the move is
                 * SWRSCAN_MIN_MS old and the power has not gone up for SWRSCAN_FLAT_MS */
                if (atomic_exchange(&swrscan_fresh, false) || !pack->tx) {
                    swrscan_peak    = 0;        /* a new scan, or not keyed (yet): the wait counts from the key */
                    swrscan_move_ms = swrscan_up_ms = now_time;
                }
                if (!pack->tx)
                    break;
                if (pack->tx_power > swrscan_peak || pack->tx_power + 1 < swrscan_peak) {
                    swrscan_peak  = pack->tx_power;     /* going up, or a new ramp */
                    swrscan_up_ms = now_time;
                }
                if ((now_time - swrscan_move_ms < SWRSCAN_MIN_MS || now_time - swrscan_up_ms < SWRSCAN_FLAT_MS) &&
                    now_time - swrscan_move_ms < SWRSCAN_WAIT_MS)
                    break;
                swrscan_peak    = 0;            /* read: dialog_swrscan_update moves to the next point */
                swrscan_move_ms = swrscan_up_ms = now_time;
#endif
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

        /* Not by the CW key's hang: a paddle in the KEY jack answering a CW message within it is the base's TX */
        if (state == RADIO_TX && (hw_tx_keyed_by_gui() || morse_on)) {
            tx_by_gui = true;
        }
        if (tx_watch_check(&tx_watch, now_time, pack->tx, gui_tx)) {
            base_guard_log("base still in TX %u ms after an un-key or write error, nothing keying it from the GUI: PTT off resent",
                           TX_WATCH_HOLD_MS);
            resync_due = true;
        }

        switch (tx_watch_power(&tx_watch, now_time, pack->tx)) {
            case TX_WATCH_CUT:
                WITH_RADIO_LOCK(set_txpwr(0.0f));   /* 0 W also if a transverter's limit changes meanwhile */
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
         * only that thread may do. Posted when it changes (held down, the base repeats it);
         * last_hkey: what the GUI was told (the note above radio_tick) */
        static uint32_t last_hkey;
        if (pack->hkey != last_hkey) {
            uint32_t hkey = pack->hkey;

            if (scheduler_put_urgent(hkey_sched_cb, &hkey, sizeof(hkey))) {
                last_hkey = hkey;
            }
        }
    } else {
        if (d > FLOW_RESTART_TIMEOUT) {
            LV_LOG_WARN("Flow reset");
            static uint64_t silent_logged;
            radio_state_t   s = atomic_load(&state);

            /* The steps below act at once, their line in the base log through log_later: in the first second
             * with none yet */

            /* The tuner's steps above go by the packets: with none (a base whose packets do not decode, #152), a tune
             * asked stayed in its state for good without a word, and tx_player refused every digital TX until the GUI
             * restarted. Given up on both radios once the base has been silent for as long as a tune has to start;
             * the tune bit goes to the base only on a packet, so a base silent from the press on is never asked to tune */
            if ((s == RADIO_ATU_START || s == RADIO_ATU_WAIT || s == RADIO_ATU_RUN) &&
                now_time - pack_time > ATU_START_MS) {
                atu_give_up(s, "Tuner: no data from the base");
            }
            /* A TX ends on a packet without the TX flag: with none (the data lost while the base keyed, or while the
             * GUI did and let go since) the state stayed RADIO_TX for good, tx_player refusing every digital TX, CAT
             * and TCI reporting TX, and base_gone never saying it. Back to RX once the base has been silent this long
             * and the GUI keys nothing, its CW past the base's hang; a packet with the TX flag brings TX back. A TX
             * the GUI never keyed is the base's own (the hand mic's PTT, VOX, the KEY jack), which the GUI cannot see
             * let go: it waits as long as the notice (base_gone), the base taken for gone, not 3 s into a gap while
             * still keyed, CAT, TCI and the remote saying RX and a remote client free to take control. Nor while the
             * register block is due (resync_due): a base that hangs also NAKs on I2C, and the GUI's PTT off that
             * failed with it may not have reached it. TX is what the screen should say then; tx_watch, which resends
             * and cuts the power, needs packets. The first block that gets through clears it */
            if (s == RADIO_TX && now_time - pack_time > (tx_by_gui ? NO_DATA_MS : BASE_GONE_MS) && !gui_keys() &&
                (uint32_t)now_time - morse_ms >= MORSE_HANG_MS && !resync_due && state_move(RADIO_TX, RADIO_RX)) {
                tx_over();
                log_later("TX: no data from the base for %u ms, back to RX", (unsigned)(now_time - pack_time));
            }
            /* The SWR scan keys the base at its start and steps on the readings: with none a 1 W carrier went out
             * until Stop, the sweep standing still and the state refusing digital TX. Stopped as its Stop does, the
             * stop asked once (marked before the put: the job may run before the put returns) */
            if (s == RADIO_SWRSCAN && now_time - pack_time > NO_DATA_MS &&
                !atomic_exchange(&swrscan_stop_asked, true)) {
                if (scheduler_put_urgent(swrscan_gone_cb, NULL, 0)) {
                    log_later("SWR scan: no data from the base for %u ms, stopped", (unsigned)(now_time - pack_time));
                } else {
                    atomic_store(&swrscan_stop_asked, false);   /* the next reset tries again */
                }
            }
            /* The X6200's POWER key is in the data alone: a tap whose release fell into a gap stayed down, and its
             * hold timer (front_keys.c) made it a power-off, which the step below now carries out at once. A key
             * the data no longer reports is let go */
            if (power_was) {
                bool down = false;

                if (scheduler_put_urgent(power_key_cb, &down, sizeof(down))) {
                    power_was = false;
                }
            }
            /* The X6100's POWER key is its own, not in the data: radio_poweroff saved, synced and said "Power off",
             * and only a packet switched the radio off. With none it stayed on. The switch-off is a register write,
             * which does not need the data */
            if (s == RADIO_POWEROFF) {
                WITH_RADIO_LOCK(hw_poweroff());
                state_move(RADIO_POWEROFF, RADIO_OFF);
            }
            if (base_gone()) {
                silent_logged = now_time;   /* its line instead of a "base silent" one */
            } else if (base_guard_log_free() && base_silent_due(now_time, start_time, &silent_logged)) {
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
    block_time = now_time;
    return ok;
}

static void * radio_thread(void *arg) {
    while (true) {
        now_time = get_time();

        guard_events();
        morse_limit_check();

        if (radio_tick()) {
            usleep(2000);
        }

        int32_t idle = now_time - idle_time;

        /* Every 3 s in RX as always (every second in every state for a base that wants it, the X6200, which
         * otherwise takes the RF away about a second into TX: issues #1, #8); after a write error or an un-key
         * at once, in TX too. The X6200's second counts from the last block, not from the last write: any write,
         * even one the library skipped as unchanged, put it off, and a client setting a value more than once a
         * second kept the block from going at all (#16) */
        bool keepalive = radio_caps() & HW_CAP_KEEPALIVE;
        if ((keepalive ? now_time - block_time > KEEPALIVE_MS : (idle > IDLE_TIMEOUT && state == RADIO_RX))
            || (resync_due && now_time - resync_time >= RESYNC_RETRY_MS)) {
            resync_time = now_time;
            resync_due = !resend_all();
            if (resync_due) {
                base_guard_log("register block not sent, again in %u ms", RESYNC_RETRY_MS);
                tx_watch_error(&tx_watch, now_time);
            }
            idle_time = now_time;
        }
        open_tx_tick();     /* the X6200's open TX when due, after the keepalive's block: its 0.2 s holds none up */
    }
    return NULL;
}
