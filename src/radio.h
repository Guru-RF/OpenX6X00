/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
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

bool radio_init();     /* false: the base's pins or line could not be opened */
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
/* Both VFOs back on their own frequency as it is now, after radio_set_freq (Birdie Cal's end): the hold that
 * radio_set_freq put on lasts until this */
void radio_set_freq_dial(void);
bool radio_check_freq(int32_t freq);
/* The frequency the radio transmits on: the other VFO's in split; with a TX shift the active one's + the shift
 * (- on VFO B), also where the other VFO cannot follow (below 100 kHz), so that it is refused */
int32_t radio_tx_freq(void);
/* Where the GUI's CW key and what the base keys by itself would go out and the radio may not transmit (#121,
 * cfg_tx_refusal): radio_tx_freq, and in split the dial too where the base keys it: always on the X6200, whose base keys it whatever split says; on
 * either radio while split is held off (an app, TCI's audio, a voice message, the SWR scan) and the VFO in use is back
 * on the dial. *freq (may be NULL): the one refused, for the words. Any thread */
bool radio_key_refused(int32_t *freq);
/* The same for PTT: on the X6200 it moves the VFO in use to the TX frequency (radio_set_ptt), so in split the dial does
 * not count unless split is held off already. Any thread */
bool radio_ptt_refused(int32_t *freq);
/* The frequency the base keys now: the dial where it keys that (the X6200 in split, for what does not move the VFO in
 * use), else radio_tx_freq. For the words on the screen. Any thread */
int32_t radio_key_freq(void);
/* Why the radio does not transmit on freq, on the screen (#121): "<what> on 27.185 MHz", then "Out of band:
 * Settings > TX Cal" or "Receive only there", each a line the message box shows whole in every font (DejaVu, the
 * widest: 570 px at most of about 580), all but "Scan stopped on" 100 MHz and up with 1 Hz digits (592 px: it
 * scrolls a little); the frequency, the reason and the setting in one line scrolled out before the setting showed.
 * now: over what shows, else after the messages waiting. False: it may transmit there, nothing said. Any thread */
bool radio_say_no_tx(const char *what, int32_t freq, bool now);
/* The base sends no data (#152): none for NO_DATA_MS, or none since the start (base_no_data). PTT, the modem, the
 * GUI's CW key, the tuner and the SWR scan are refused meanwhile. Any thread */
bool radio_no_data(void);
/* Why then, on the screen: "<what>: no data from the base", then "Baseband: Settings > Info", each whole in every
 * font (DejaVu 38: 536 px at most for "No tune"). now: over what shows, else after the messages waiting. False: the
 * data flows, nothing said. Any thread */
bool radio_say_no_data(const char *what, bool now);
/* The radio transmits on `freq`, without updating a subject: a digital app's tone, the SWR scan. The active VFO
 * moves; in split the base's split bit is off until 0, which puts both VFOs back on their own frequency, IF shift
 * and all, and split back on */
void radio_set_tx_freq(int32_t freq);

radio_vfo_t radio_toggle_vfo();

uint16_t radio_change_vol(int16_t df);
bool radio_change_spmode(int16_t df);

void radio_change_mute();
/* A TCI client transmits: the speaker and the monitor quiet until it unkeys */
void radio_set_tci_mute(bool on);
/* The scanner looking from one frequency to the next: the speaker quiet until it stops */
void radio_set_scan_mute(bool on);
void radio_vol_refresh();

void radio_set_pwr(float d);
/* While a digital app is open the radio never goes above TX_PLAYER_MAX_PWR_W */
void radio_set_digi_cap(bool on);
/* A digital app is open (radio_set_digi_cap) */
bool radio_digi_cap(void);
/* TX audio filter of the baseband, Hz. Not saved: cfg_tx_filter_low/high stay as they are */
void radio_set_tx_filter(uint16_t from_hz, uint16_t to_hz);
/* true: NR, NB, DNF and auto DNF off in the baseband (it uses them in DIGI modes too);
 * false: back to what the settings say. Not saved */
void radio_set_rx_dsp_off(bool all_off);

void radio_set_charger(bool on);

void radio_start_atu();

/* A TUNE for an external tuner (tune_tx.c) is on the air at w W for up to limit_ms: the TX power never above w
 * meanwhile, whatever sets it, and the SWR protection off for the readings of the tune's own power within its time
 * (tune_swr_limit). 0: over, the power back to the setting. Any thread */
void radio_set_tune(float w, uint32_t limit_ms);
/* The base's mode for a transmission on the VFO that transmits (tx_player's), without the setting; -1: both VFOs
 * back to their own. Any thread */
void radio_set_tx_mode(int mode);
/* What radio_set_tx_mode set for the transmission under way, -1 none. Any thread */
int  radio_tx_mode(void);

/* From RX only, where the radio may transmit and with data from the base (else false, "No TX on ..." or "No TX: no
 * data from the base" said): the VFO that transmits to `from`, then AM at 1 W at most */
bool radio_start_swrscan(int32_t from);
void radio_stop_swrscan();

void radio_poweroff();
/* PTT from the GUI. False: a key down refused where the radio may not transmit (the screen says why, #121), or while
 * the base sends no data (#152) */
bool radio_set_ptt(bool tx);
/* The GUI's PTT is on (radio_set_ptt), whoever set it: the PTT key, the browser's, CAT, TCI, the keyboard's. Any
 * thread */
bool radio_get_ptt(void);
/* The mic of the baseband without saving it (radio_mic_sel_t), -1: back to cfg_mic */
void radio_set_mic_override(int mic);
/* The mic in use: the override, else cfg_mic */
int radio_get_mic(void);
void radio_set_modem(bool tx);
bool radio_get_modem(void);   /* the TX audio from the GUI (apps, recordings), not the mic */
/* How many times the SWR protection stopped a transmission since the start. Any thread */
unsigned radio_swr_trips(void);
/* How many times the radio stopped the GUI's own TX where it may not transmit (#121: moved outside the amateur bands
 * while keyed, or the switch turned off), held at 0 W before. Any thread */
unsigned radio_band_stops(void);
/* When it last did, get_time() (CLOCK_MONOTONIC ms), 0 never; set before the count goes up. Any thread */
uint64_t radio_swr_trip_at(void);
/* true: the baseband sends our audio to the speaker, the path of voice prompts and the
 * recorder (meanwhile its audio to us is not the receiver); false: the mics again */
void radio_speaker_play(bool speaker);
/* What the base streams: the receiver, our audio to the speaker, or the hand mic and our audio */
void radio_set_play_mode(audio_play_mode_t mode);

/* The base's firmware version, and as the base prints it */
radio_base_ver_t radio_base_ver(void);
const char *radio_base_ver_str(void);
#ifdef BOARD_X6200
/* The PA bias sent and where it came from, for Settings > Info (pa_bias.h) */
void radio_pa_bias_info(char *buf, size_t size);
#endif
/* HW_CAP_* bits (radio_types.h): what the base does itself */
uint32_t radio_caps(void);
/* CW and CW-R as the base receives them: 1 when the tone rises as the dial goes up (CW-R; CW too with
 * HW_CAP_CW_TONE_RISES, the X6200), -1 when it falls (CW on the X6100), 0 in the other modes. The tone is
 * heard from an oscillator the key tone off the dial on that side: a station on the dial sounds at the key tone */
int radio_cw_tone_dir(radio_mode_t mode);

/* The board's switches on the base's pins */
void radio_wifi_power(bool on);
void radio_backlight_power(bool on);
void radio_usb_to_cat(void);

void radio_set_line_in(uint8_t d);
void radio_set_line_out(uint8_t d);

/* The GUI's CW key. False: a key down refused where the radio may not transmit (the screen says why, #121), or while
 * the base sends no data (#152) */
bool radio_set_morse_key(bool on);
/* The GUI's CW key down for at most ms at a time, 0: no limit (QRSS3's TX time limit): past it the radio thread lets
 * go of the key and counts it. Any thread */
void radio_set_morse_key_limit(uint32_t ms);
unsigned radio_morse_key_cuts(void);
/* Break-in delay of the base, ms (cfg_qsk_time is the setting; the CW encoder holds TX longer) */
void radio_set_qsk_time(uint16_t ms);
/* The TX voice reaches the screen recording only through the monitor: at least 30 while recording */
void radio_set_rec_moni(bool on);

int8_t radio_get_audio_in_lvl();

/* The battery % as on the screen, -1 on external power or before the base measured it. Any thread. */
int radio_get_battery();
