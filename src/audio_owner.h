/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - who has the speaker. The apps that play long audio of their own (the Internet radio's station, a voice
 *  link's messages: Zello, later SVXConnect) each claim it while they play and release it after, each on a stream of
 *  its own (audio.h, audio_user_*). From the claims:
 *  - the speaker's play mode: Play from the first claim on the speaker to the last release, Rec while a voice link
 *    records the hand mic, else the receiver's own;
 *  - the receiver in the Bluetooth headphones: muted from the first claim on the headphones to the last release;
 *  - a voice link wins over the station: while a link holds a claim (a message playing, or the hand mic recorded)
 *    the station's audio is dropped, and it goes on when the link has released.
 *  The rules alone, without audio: audio.c applies them under its lock, tools/audio_owner tests them.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t claims[AUDIO_USERS];    /* each user's claims: they nest, released one by one */
    bool    rec[AUDIO_USERS];       /* the user records the hand mic (a voice link's message to send) */
    bool    phones[AUDIO_USERS];    /* the user's stream is on the Bluetooth headphones, not the speaker */
} audio_owner_t;

typedef struct {
    audio_play_mode_t mode;         /* the speaker's */
    bool              rx_mute;      /* the receiver muted in the headphones */
} audio_owner_out_t;

/* Zello and SVXConnect: their claims win over the station's */
bool audio_owner_is_link(audio_user_t u);
/* Where the user's stream plays, from its opening on */
void audio_owner_place(audio_owner_t *o, audio_user_t u, bool phones);
/* One claim more. True when it is a voice link's that the station gives way to now: the station's queue to drop */
bool audio_owner_claim(audio_owner_t *o, audio_user_t u);
/* One claim less; none left, nothing */
void audio_owner_release(audio_owner_t *o, audio_user_t u);
/* All its claims and its Rec: a key going down, a stream closing */
void audio_owner_release_all(audio_owner_t *o, audio_user_t u);
/* The hand mic recorded for a voice link, or no more. True as a claim is: the station's queue to drop */
bool audio_owner_rec(audio_owner_t *o, audio_user_t u, bool on);
/* What u writes is dropped: a voice link has a claim or Rec while u is the station */
bool audio_owner_held(const audio_owner_t *o, audio_user_t u);
/* The speaker's mode and the headphones' receiver as the claims want them */
audio_owner_out_t audio_owner_out(const audio_owner_t *o);

#ifdef __cplusplus
}
#endif
