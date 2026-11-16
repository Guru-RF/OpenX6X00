/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Zello app's two rules, apart from the GUI so a PC can test them (tools/zello):
 *
 *  - The radio's PTT key: while the app is open it talks on the Zello channel and never keys the radio; closed, it
 *    keys the radio as always. A press keeps its owner until its release, so a release always goes where its press
 *    went: a press that keyed the radio before the app opened unkeys it, one held for Zello when the app closes keys
 *    nothing, and the next press keys the radio.
 *  - The base's audio path for Zello: off (as the radio has it) while the radio transmits, whatever keyed it, and for
 *    a while after; the hand mic into Linux while talking; Zello's audio on the speaker while on a channel.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZELLO_KEY_RADIO,        /* the radio's PTT (ptt_key in screen_main.c) */
    ZELLO_KEY_ZELLO,        /* the app's talk (dialog_zello_ptt) */
} zello_key_t;

typedef struct {
    bool        down;       /* a press not released yet */
    zello_key_t owner;      /* where that press went */
} zello_ptt_t;

/* A press of the PTT key: where it goes. A press while one that keyed the radio is still held (its release lost)
 * stays the radio's */
zello_key_t zello_ptt_press(zello_ptt_t *p, bool app_open);
/* Its release: where its press went; the radio's when no press was seen (a release that unkeys nothing) */
zello_key_t zello_ptt_release(zello_ptt_t *p);

#define ZELLO_AIR_HOLD_MS 500   /* after a transmission: the TX audio's tail, the base back on receive */

typedef enum {
    ZELLO_PATH_OFF,         /* AUDIO_PLAY_OFF: the radio's own path, Zello's audio dropped */
    ZELLO_PATH_LISTEN,      /* AUDIO_PLAY_ON: Zello's audio on the speaker, while it plays */
    ZELLO_PATH_TALK,        /* AUDIO_PLAY_VOICE_REC: the hand mic into Linux, for the channel */
} zello_path_t;

typedef struct {
    bool     on_air;        /* the last look saw the radio transmit */
    uint64_t air_end_ms;    /* when it stopped */
} zello_gate_t;

/* The path now. on_air: the radio transmits or the GUI keys it (its state, its PTT, its modem, the moment before a
 * key from the GUI); talk: a message from the hand mic is wanted; playing: the channel's audio to hear (a talker, or
 * audio a moment ago); connected: on a channel. Off while on the air and ZELLO_AIR_HOLD_MS after, and while nothing
 * plays: then the speaker is the receiver's */
zello_path_t zello_path(zello_gate_t *g, bool connected, bool talk, bool playing, bool on_air, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
