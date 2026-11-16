/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the internet voice apps' two rules (Zello, SVXConnect), apart from the GUI so a PC can test them
 *  (tools/zello, tools/svx):
 *
 *  - The radio's PTT key: while the app is open it talks on the network (a Zello channel, a reflector's talkgroup) and
 *    never keys the radio; closed, it keys the radio as always. A press keeps its owner until its release, so a
 *    release always goes where its press went: a press that keyed the radio before the app opened unkeys it, one held
 *    for the app when it closes keys nothing, and the next press keys the radio.
 *  - The base's audio path for the app: off (as the radio has it) while the radio transmits, whatever keyed it, and
 *    for a while after; the hand mic into Linux while talking; the network's audio on the speaker while connected.
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
    VPTT_RADIO,             /* the radio's PTT (ptt_key in screen_main.c) */
    VPTT_APP,               /* the app's talk (vnet_ptt_key) */
} vptt_key_t;

typedef struct {
    bool        down;       /* a press not released yet */
    vptt_key_t  owner;      /* where that press went */
} vptt_t;

/* A press of the PTT key: where it goes. A press while one that keyed the radio is still held (its release lost)
 * stays the radio's */
vptt_key_t vptt_press(vptt_t *p, bool app_open);
/* Its release: where its press went; the radio's when no press was seen (a release that unkeys nothing) */
vptt_key_t vptt_release(vptt_t *p);

#define VOICE_AIR_HOLD_MS 500   /* after a transmission: the TX audio's tail, the base back on receive */

typedef enum {
    VPATH_OFF,              /* AUDIO_PLAY_OFF: the radio's own path, the network's audio dropped */
    VPATH_LISTEN,           /* AUDIO_PLAY_ON: the network's audio on the speaker, while it plays */
    VPATH_TALK,             /* AUDIO_PLAY_VOICE_REC: the hand mic into Linux, for the network */
} vpath_t;

typedef struct {
    bool     on_air;        /* the last look saw the radio transmit */
    uint64_t air_end_ms;    /* when it stopped */
} vgate_t;

/* The path now. on_air: the radio transmits or the GUI keys it (its state, its PTT, its modem, the moment before a
 * key from the GUI); talk: a message from the hand mic is wanted; playing: the network's audio to hear (a talker, or
 * audio a moment ago); connected: on the network (a channel, a reflector). Off while on the air and VOICE_AIR_HOLD_MS
 * after, and while nothing plays: then the speaker is the receiver's */
vpath_t vpath(vgate_t *g, bool connected, bool talk, bool playing, bool on_air, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
