/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "radio_types.h"

#include "lvgl/lvgl.h"
#include "audio.h"

#define RADIO_SAMPLES   (512)

typedef enum {
    RADIO_RX = 0,
    RADIO_TX,
    RADIO_ATU_START,
    RADIO_ATU_WAIT,
    RADIO_ATU_RUN,
    RADIO_SWRSCAN,

    RADIO_POWEROFF,
    RADIO_OFF
} radio_state_t;

typedef enum {
    RADIO_CHARGER_OFF = 0,
    RADIO_CHARGER_ON,
    RADIO_CHARGER_SHADOW
} radio_charger_t;

typedef void (*radio_rx_tx_change_t) (bool tx);

void radio_init();
void radio_start();
void radio_set_rx_tx_notify_fn(radio_rx_tx_change_t cb);
void radio_set_low_power_cb(void(*cb)(bool));
void radio_bb_reset();
radio_state_t radio_get_state();

/**
 * Set freq for radio without updating corresponding subject: the active VFO, which receives
 * (Birdie Cal's scan). The apps' tones and the SWR scan go to radio_set_tx_freq
 */
void radio_set_freq(int32_t freq);
bool radio_check_freq(int32_t freq);
/* The frequency the radio transmits on: the other VFO's in split (a TX shift keeps it there) */
int32_t radio_tx_freq(void);
/* The VFO the radio transmits on (the other one in split) to `freq` without updating its subject: a digital app's
 * tone, the SWR scan. 0: back to its own frequency, IF shift and all */
void radio_set_tx_freq(int32_t freq);

radio_vfo_t radio_toggle_vfo();

uint16_t radio_change_vol(int16_t df);
bool radio_change_spmode(int16_t df);

void radio_change_mute();
/* A TCI client transmits: the speaker and the monitor quiet until it unkeys */
void radio_set_tci_mute(bool on);
void radio_vol_refresh();

void radio_set_pwr(float d);
/* While a digital app is open the radio never goes above TX_PLAYER_MAX_PWR_W */
void radio_set_digi_cap(bool on);
/* TX audio filter of the baseband, Hz. Not saved: cfg_tx_filter_low/high stay as they are */
void radio_set_tx_filter(uint16_t from_hz, uint16_t to_hz);
/* true: NR, NB, DNF and auto DNF off in the baseband (it uses them in DIGI modes too);
 * false: back to what the settings say. Not saved */
void radio_set_rx_dsp_off(bool all_off);

void radio_set_charger(bool on);

void radio_start_atu();

bool radio_start_swrscan();
void radio_stop_swrscan();

void radio_poweroff();
void radio_set_ptt(bool tx);
/* The mic of the baseband without saving it (radio_mic_sel_t), -1: back to cfg_mic */
void radio_set_mic_override(int mic);
/* The mic in use: the override, else cfg_mic */
int radio_get_mic(void);
void radio_set_modem(bool tx);
bool radio_get_modem(void);   /* the TX audio from the GUI (apps, recordings), not the mic */
/* How many times the SWR protection stopped a transmission since the start. Any thread */
unsigned radio_swr_trips(void);
/* true: the baseband sends our audio to the speaker, the path of voice prompts and the
 * recorder (meanwhile its audio to us is not the receiver); false: the mics again */
void radio_speaker_play(bool speaker);
/* What the base streams: the receiver, our audio to the speaker, or the hand mic and our audio */
void radio_set_play_mode(audio_play_mode_t mode);

/* The base's firmware version, and as the base prints it */
radio_base_ver_t radio_base_ver(void);
const char *radio_base_ver_str(void);
/* HW_CAP_* bits (radio_types.h): what the base does itself */
uint32_t radio_caps(void);

/* The board's switches on the base's pins */
void radio_wifi_power(bool on);
void radio_backlight_power(bool on);
void radio_usb_to_cat(void);

void radio_set_line_in(uint8_t d);
void radio_set_line_out(uint8_t d);

void radio_set_morse_key(bool on);
/* Break-in delay of the base, ms (cfg_qsk_time is the setting; the CW encoder holds TX longer) */
void radio_set_qsk_time(uint16_t ms);
/* The TX voice reaches the screen recording only through the monitor: at least 30 while recording */
void radio_set_rec_moni(bool on);

int8_t radio_get_audio_in_lvl();

/* The battery % as on the screen, -1 on external power or before the base measured it. Any thread. */
int radio_get_battery();
