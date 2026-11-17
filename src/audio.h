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

/* The apps that play long audio of their own, each on a stream of its own, and who has the speaker meanwhile
 * (audio_owner.h): the speaker in Play mode from the first claim to the last release (the receiver muted in the
 * Bluetooth headphones while they stand in for it), and a voice link's message over a listener (the station, DMR):
 * the listener's audio is dropped while a link holds a claim and goes on after it */
typedef enum {
    AUDIO_USER_STATION,     /* a listener: the Internet radio (dialog_iradio.c), through audio_listen_* */
    AUDIO_USER_ZELLO,       /* a voice link: Zello's channel (zello.c) */
    AUDIO_USER_SVX,         /* a voice link: SVXConnect's */
    AUDIO_USER_DMR,         /* a listener: DMR's talkgroups (dialog_dmr.c), through audio_listen_* */
    AUDIO_USERS
} audio_user_t;

/* From the user's playing thread: its stream, on the headphones while they stand in for the speaker, else the codec's.
 * 1 on the headphones, 0 on the speaker, -1 none (what it writes is not heard). Close: its claims released, the
 * stream and what it queued gone */
int audio_user_open(audio_user_t u);
void audio_user_close(audio_user_t u);
/* While it plays (any thread): claims nest, the speaker or the headphones are given back at the last release */
void audio_user_claim(audio_user_t u);
void audio_user_release(audio_user_t u);
/* A voice link recording the hand mic for its message: the speaker in Rec mode until off (any thread) */
void audio_user_rec(audio_user_t u, bool on);
/* What fits now of a write, the samples taken, from its thread: in pieces of about a thousand samples, a stream never
 * has room for much more. While held (a voice link over the station) all is taken and dropped */
size_t audio_user_write(audio_user_t u, const int16_t *buf, size_t samples);
/* What it has queued is dropped, its own and no other's (any thread) */
void audio_user_flush(audio_user_t u);

/* The listeners, the Internet radio's station (AUDIO_USER_STATION) and DMR (AUDIO_USER_DMR), each from its playing
 * thread: begin as it starts; open as its first audio comes (its stream and its claim: as audio_user_open); play
 * writes in pieces of at most 1024 samples, each one after go_on() said yes, and returns 0 when all is written, -1
 * when go_on() said no or a key cut it, -2 when the stream had no room for timeout_ms, -3 when the write failed; while
 * a voice link holds the speaker the pieces are dropped at the pace they would have played. End: what is still queued
 * dropped, the claim and the stream gone.
 * DMR opens it a call at a time and ends it after the call's last audio has played: between calls the speaker is the
 * receiver's.
 * audio_listen_cut(), from any thread: what every listener queued is dropped, its claims released at once and it
 * writes nothing more until its next begin, or a resume once no cut has come for quiet_ms (the listener that goes
 * on after the radio's TX: true when it may play again). radio.c calls the cut before its PTT and its modem key, so
 * a listener never plays into what the radio sends */
void audio_listen_begin(audio_user_t u);
int audio_listen_open(audio_user_t u);
bool audio_listen_resume(audio_user_t u, int quiet_ms);
int audio_listen_play(audio_user_t u, const int16_t *buf, size_t samples, bool (*go_on)(void), int timeout_ms);
void audio_listen_end(audio_user_t u);
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
