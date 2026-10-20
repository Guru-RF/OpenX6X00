/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <ft8lib/constants.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <aether_radio/x6100_control/control.h>
#include "../radio.h"
#include "../clock.h"
#include "common.h"
#include "types.h"

typedef enum {
    BUTTONS_DARK = 0,
    BUTTONS_LIGHT,
    BUTTONS_TEMPORARILY
} buttons_light_t;

typedef enum {
    ACTION_NONE = 0,
    ACTION_SCREENSHOT,
    ACTION_RECORDER,
    ACTION_MUTE,
    ACTION_STEP_UP,
    ACTION_STEP_DOWN,
    ACTION_VOICE_MODE,  /* unused (voice guidance removed), kept: values are stored in settings */
    ACTION_BAT_INFO,    /* unused (spoken battery info removed) */
    ACTION_NR_TOGGLE,
    ACTION_NB_TOGGLE,
    ACTION_WF_MODE,     /* appended: waterfall Centered/Fixed toggle */
    ACTION_SCREEN_REC,  /* appended after WF_MODE: screen + audio recording on/off */
    ACTION_ATT_TOGGLE,  /* appended after SCREEN_REC */
    ACTION_PRE_TOGGLE,
    ACTION_ATU_TOGGLE,  /* appended after PRE_TOGGLE */
    ACTION_ATU_TUNE,    /* start a tuning cycle: transmits a carrier */

    ACTION_APP_RTTY = 100,
    ACTION_APP_FT8,
    ACTION_APP_SWRSCAN,
    ACTION_APP_GPS,
    ACTION_APP_SETTINGS,
    ACTION_APP_RECORDER,
    ACTION_APP_QTH,
    ACTION_APP_CALLSIGN,
    ACTION_APP_WIFI,
    ACTION_APP_WEFAX, ACTION_APP_NAVTEX, ACTION_APP_JS8,  /* from JS8 on, appended: the values are stored in settings */
    ACTION_APP_SSTV,    /* appended after JS8 */
    ACTION_APP_CLUSTER, /* appended after SSTV */
    ACTION_APP_MODES,   /* appended after CLUSTER */
    ACTION_APP_CWTRAINER, /* appended after MODES */
    ACTION_APP_WSPR,    /* appended after CWTRAINER */
    ACTION_APP_CW_KBD,  /* appended after WSPR */
    ACTION_APP_FIELD, /* appended after CW_KBD: Field Ops (POTA/SOTA) */
    ACTION_APP_BANDS, /* appended after FIELD: the band overview of Field Ops alone */
    ACTION_APP_SNAKE, /* appended after BANDS: the hidden game (long press on APP > GPS) */
    ACTION_APP_PSK,     /* 121, appended after SNAKE: PSK31/PSK63 (upstream's 114/115 are MODES/CWTRAINER here) */
    ACTION_APP_OLIVIA,  /* 122 */
    ACTION_APP_JTTY,    /* 123, appended after OLIVIA */
    ACTION_APP_CHANNELS, /* 124: the CHANNELS window, as the hand mic V/M key */
} press_action_t;

typedef enum {
    FREQ_ACCEL_NONE = 0,
    FREQ_ACCEL_LITE,
    FREQ_ACCEL_STRONG,
} freq_accel_t;

/* Themes */
typedef enum {
    THEME_SIMPLE,
    THEME_LEGACY,
    THEME_OUTDOOR,
    THEME_CLASSIC,
} themes_t;


/* Params */

typedef struct {
    /* LCD */

    int16_t             brightness_normal;
    int16_t             brightness_idle;
    uint16_t            brightness_timeout; /* seconds */
    buttons_light_t     brightness_buttons;

    /* radio */

    params_uint8_t      charger;
    uint16_t            bias_drive;
    uint16_t            bias_final;
    uint8_t             line_in;
    uint8_t             line_out;
    params_bool_t       spmode;
    params_uint8_t      freq_accel;
    params_uint8_t      screen_rec_split;   /* file length: 60, 45, 30, 15, 5 min */

    /* main screen */

    params_uint8_t      spectrum_beta;
    params_bool_t       spectrum_peak;
    params_uint8_t      spectrum_peak_hold;
    params_uint8_t      spectrum_peak_speed;
    params_bool_t       spectrum_filled;
    params_bool_t       waterfall_smooth_scroll;
    params_bool_t       waterfall_center_line;
    params_bool_t       waterfall_zoom;
    params_bool_t       mag_freq;
    params_bool_t       mag_info;
    params_bool_t       mag_alc;
    clock_view_t        clock_view;
    uint8_t             clock_time_timeout;     /* seconds */
    uint8_t             clock_power_timeout;    /* seconds */
    uint8_t             clock_tx_timeout;       /* seconds */

    /* Msg */

    uint16_t            cw_encoder_period;  /* seconds */
    uint16_t            voice_msg_period;   /* seconds */

    /* RTTY */

    uint16_t            rtty_center;
    uint16_t            rtty_shift;
    uint32_t            rtty_rate;
    bool                rtty_reverse;
    uint8_t             rtty_bits;
    float               rtty_snr;

    /* FT8 */

    params_uint16_t     ft8_tx_freq, js8_tx_freq;
    params_bool_t       js8_hold_offset, js8_auto, js8_hb, js8_hb_ack;
    params_bool_t       js8_relay;       /* relays (>) passed on, MSG TO: held for others, as JS8Call does */
    params_uint16_t     js8_hb_interval, js8_cq_interval;  /* minutes; auto CQ counts from the end of each CQ */
    params_bool_t       js8_log_prompt;  /* the log form offered when a QSO ends */
    params_uint8_t      js8_log_activation; /* activation logged: 0 none, 1 POTA, 2 SOTA */
    params_uint8_t      js8_alerts;      /* JS8_ALERT_* bits in js8_window.c */
    params_uint8_t      js8_speed;       /* the js8_speed_t sent at */
    params_bool_t       js8_rx_all;      /* every speed decoded, as JS8Call's multi-decoder */
    params_bool_t       js8_ghostnet;    /* band keys go through the GhostNet frequencies */
    params_bool_t       js8_custom_on;   /* on js8_custom_hz rather than a preset */
    params_int32_t      js8_custom_hz;   /* the last custom dial frequency, 0 none */
    params_bool_t       ft8_custom_on;   /* FT8/FT4/FT2 tuned to ft8_custom_hz, not a preset */
    params_int32_t      ft8_custom_hz;   /* last custom dial frequency, 0 = none */
    params_str_t        ft8_cq_modifier;
    params_bool_t       sstv_denoise;    /* clean received pictures, keep the original too */

    // FT8 gain offset for different radios/bands/modes
    params_float_t      ft8_output_gain_offset;

    /* Long press actions */

    uint8_t             long_gen;
    uint8_t             long_app;
    uint8_t             long_key;
    uint8_t             long_msg;
    uint8_t             long_dfn;
    uint8_t             long_dfl;

    /* HMic F1, F2 actions */

    uint8_t             press_f1;
    uint8_t             press_f2;
    uint8_t             long_f1;
    uint8_t             long_f2;

    /* Audio play/rec */

    params_float_t      play_gain_db_f;
    params_float_t      rec_gain_db_f;

    params_str_t        qth;
    params_str_t        callsign;

    /* WiFi / BT */

    params_bool_t       wifi_enabled;

    /* Theme */

    params_uint8_t       theme;         /* old setting, replaced by theme_name */
    params_str_t         theme_name;    /* built-in theme or /mnt/themes/<name>.json */
    params_uint8_t       font;

    /* durty flags */

    struct {
        bool    vol_modes;
        bool    mfk_modes;

        bool    brightness_normal;
        bool    brightness_idle;
        bool    brightness_timeout;
        bool    brightness_buttons;

        bool    mic;
        bool    hmic;
        bool    imic;
        bool    line_in;
        bool    line_out;
        bool    moni;

        bool    vox;
        bool    vox_ag;
        bool    vox_delay;
        bool    vox_gain;

        bool    clock_view;
        bool    clock_time_timeout;
        bool    clock_power_timeout;
        bool    clock_tx_timeout;

        bool    cw_encoder_period;
        bool    voice_msg_period;

        bool    rtty_center;
        bool    rtty_shift;
        bool    rtty_rate;
        bool    rtty_reverse;

        bool    long_gen;
        bool    long_app;
        bool    long_key;
        bool    long_msg;
        bool    long_dfn;
        bool    long_dfl;

        bool    press_f1;
        bool    press_f2;
        bool    long_f1;
        bool    long_f2;

    } dirty;
} params_t;


extern params_t params;

void params_init();
void params_flush();

void params_bool_set(params_bool_t *var, bool x);
void params_uint8_set(params_uint8_t *var, uint8_t x);
void params_uint16_set(params_uint16_t *var, uint16_t x);
void params_int32_set(params_int32_t *p, int32_t value);
void params_str_set(params_str_t *var, const char *x);
void params_float_set(params_float_t *var, float x);

void params_msg_cw_load();
/* The MSG > CW texts in their order (the web page writes them too), at most max */
#define PARAMS_MSG_CW_LEN 65
int params_msg_cw_list(char (*out)[PARAMS_MSG_CW_LEN], int max);
void params_msg_cw_new(const char *val);
void params_msg_cw_edit(uint32_t id, const char *val);
void params_msg_cw_delete(uint32_t id);

char *params_key_mode_str_get(x6100_key_mode_t val);

char *params_iambic_mode_str_ger(x6100_iambic_mode_t val);
char *params_comp_str_get(uint8_t comp);


#ifdef __cplusplus
}
#endif
