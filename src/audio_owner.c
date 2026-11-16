/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - who has the speaker: the rules (audio_owner.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "audio_owner.h"

bool audio_owner_is_link(audio_user_t u) {
    return u == AUDIO_USER_ZELLO || u == AUDIO_USER_SVX;
}

/* A voice link with a claim or Rec, other than u */
static bool link_busy(const audio_owner_t *o, audio_user_t but) {
    for (int u = 0; u < AUDIO_USERS; u++) {
        if (u != (int)but && audio_owner_is_link((audio_user_t)u) && (o->claims[u] || o->rec[u])) return true;
    }
    return false;
}

void audio_owner_place(audio_owner_t *o, audio_user_t u, bool phones) {
    if (u < AUDIO_USERS) o->phones[u] = phones;
}

/* The link u starting to hold the speaker while the station plays and no other link held it already */
static bool station_gives_way(const audio_owner_t *o, audio_user_t u) {
    return audio_owner_is_link(u) && !o->claims[u] && !o->rec[u] && !link_busy(o, u) &&
           o->claims[AUDIO_USER_STATION];
}

bool audio_owner_claim(audio_owner_t *o, audio_user_t u) {
    if (u >= AUDIO_USERS) return false;
    bool drop = station_gives_way(o, u);

    if (o->claims[u] < UINT8_MAX) o->claims[u]++;
    return drop;
}

void audio_owner_release(audio_owner_t *o, audio_user_t u) {
    if (u < AUDIO_USERS && o->claims[u]) o->claims[u]--;
}

void audio_owner_release_all(audio_owner_t *o, audio_user_t u) {
    if (u >= AUDIO_USERS) return;
    o->claims[u] = 0;
    o->rec[u]    = false;
}

bool audio_owner_rec(audio_owner_t *o, audio_user_t u, bool on) {
    if (u >= AUDIO_USERS) return false;
    bool drop = on && station_gives_way(o, u);

    o->rec[u] = on;
    return drop;
}

bool audio_owner_held(const audio_owner_t *o, audio_user_t u) {
    return u == AUDIO_USER_STATION && link_busy(o, u);
}

audio_owner_out_t audio_owner_out(const audio_owner_t *o) {
    audio_owner_out_t out = {AUDIO_PLAY_OFF, false};
    bool              rec = false, speaker = false;

    for (int u = 0; u < AUDIO_USERS; u++) {
        rec |= o->rec[u];
        if (o->claims[u]) {
            if (o->phones[u]) out.rx_mute = true;
            else speaker = true;
        }
    }
    /* Rec plays this app's audio as well (audio.h): it covers a claim on the speaker */
    out.mode = rec ? AUDIO_PLAY_VOICE_REC : speaker ? AUDIO_PLAY_ON : AUDIO_PLAY_OFF;
    return out;
}
