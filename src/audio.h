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

#include "radio_types.h"

#define AUDIO_PLAY_RATE     (44100)
#define AUDIO_CAPTURE_RATE  (44100)
// #define AUDIO_PLAY_RATE     (22050)
// #define AUDIO_CAPTURE_RATE  (22050)

typedef enum {
    AUDIO_PLAY_OFF, // No play, BASE will not stream audio from mic or this app
    AUDIO_PLAY_ON, // Play mode, BASE will stream audio from this app. Both mic gain set to minimal
    AUDIO_PLAY_VOICE_REC  // Rec mode, BASE will stream audio from hmic and this app
} audio_play_mode_t;

/* The GUI's exit code when PulseAudio cannot be reached or goes away: x6100_daemon starts it
 * again and does not count the start against a pending update */
#define AUDIO_EXIT_NO_SERVER 3

void audio_init();
void audio_mixer_setup(radio_base_ver_t base_ver);
int audio_play(int16_t *buf, size_t samples);
void audio_play_wait();
void audio_play_flush();

/* Local playback on the Bluetooth headphones, from the playing thread (see audio.c) */
bool audio_play_bt_begin(void);
void audio_play_bt_end(void);

/* Long listening on the speaker or the headphones (the Internet radio, dialog_iradio.c), from its playing thread:
 * begin as it starts; play writes in pieces of at most 1024 samples into this thread's stream (the headphones' after
 * audio_play_bt_begin, else the speaker's in AUDIO_PLAY_ON), each one after go_on() said yes, and returns 0 when all
 * is written, -1 when go_on() said no or a key cut it, -2 when the stream had no room for timeout_ms, -3 when the
 * write failed; end (what is still queued dropped) before audio_play_bt_end and the play mode off. audio_listen_cut(),
 * from any thread: what it queued is dropped and it writes nothing more until its next begin. radio.c calls it before
 * its PTT and its modem key, so a station never plays into what the radio sends */
void audio_listen_begin(void);
int audio_listen_play(const int16_t *buf, size_t samples, bool (*go_on)(void), int timeout_ms);
void audio_listen_end(void);
void audio_listen_cut(void);

/* Short-delay playback into the codec for the phone's voice, from the playing thread (see audio.c) */
bool audio_play_lowlat_begin(void);
void audio_play_lowlat_end(void);
bool audio_play_if_room(int16_t *buf, size_t samples);

void audio_gain_db(int16_t *buf, size_t samples, float gain, int16_t *out);
void audio_gain_db_transition(int16_t *buf, size_t samples, float gain1, float gain2, int16_t *out);

float audio_set_play_vol(float db);
float audio_set_rec_vol(float db);

/* The input level for the Recorder window's meter: watched from on until off, -60 dB meanwhile and after (audio.c) */
void audio_peak_watch(bool on);
float audio_get_peak_db();
