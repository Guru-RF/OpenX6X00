/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of who has the speaker (src/audio_owner.c): the speaker's play mode and the headphones' receiver mute
 *  from the first claim to the last release, claims that nest, a key's release of all; a voice link's message over
 *  the Internet radio's station (the station's queue dropped once as the message starts, its audio dropped while the
 *  message plays, on again after), and a timeline of both writing that never alternates their pieces. `make` in
 *  tools/audio_owner.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "audio_owner.h"

#include <stdio.h>
#include <string.h>

static int failed, checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            failed++;                                                                   \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                               \
    } while (0)

static audio_play_mode_t mode(const audio_owner_t *o) {
    return audio_owner_out(o).mode;
}

static bool rx_mute(const audio_owner_t *o) {
    return audio_owner_out(o).rx_mute;
}

/* Nobody: the receiver's own speaker */
static void test_idle(void) {
    audio_owner_t o = {0};

    CHECK(mode(&o) == AUDIO_PLAY_OFF);
    CHECK(!rx_mute(&o));
    CHECK(!audio_owner_held(&o, AUDIO_USER_STATION));
    audio_owner_release(&o, AUDIO_USER_ZELLO);          /* a release with no claim: nothing */
    CHECK(o.claims[AUDIO_USER_ZELLO] == 0);
    CHECK(mode(&o) == AUDIO_PLAY_OFF);
}

/* Play from the first claim on the speaker to the last release, whoever releases first */
static void test_counting(void) {
    audio_owner_t o = {0};

    audio_owner_claim(&o, AUDIO_USER_STATION);
    CHECK(mode(&o) == AUDIO_PLAY_ON);
    audio_owner_claim(&o, AUDIO_USER_ZELLO);
    CHECK(mode(&o) == AUDIO_PLAY_ON);
    audio_owner_release(&o, AUDIO_USER_STATION);        /* the station stops under the message: Zello's still on */
    CHECK(mode(&o) == AUDIO_PLAY_ON);
    audio_owner_release(&o, AUDIO_USER_ZELLO);
    CHECK(mode(&o) == AUDIO_PLAY_OFF);

    /* Claims nest */
    audio_owner_claim(&o, AUDIO_USER_ZELLO);
    audio_owner_claim(&o, AUDIO_USER_ZELLO);
    audio_owner_release(&o, AUDIO_USER_ZELLO);
    CHECK(mode(&o) == AUDIO_PLAY_ON);
    audio_owner_release(&o, AUDIO_USER_ZELLO);
    CHECK(mode(&o) == AUDIO_PLAY_OFF);
    audio_owner_release(&o, AUDIO_USER_ZELLO);          /* one too many: still none, never below */
    audio_owner_claim(&o, AUDIO_USER_ZELLO);
    CHECK(mode(&o) == AUDIO_PLAY_ON);
    audio_owner_release(&o, AUDIO_USER_ZELLO);
    CHECK(mode(&o) == AUDIO_PLAY_OFF);

    /* A key going down: all of a user's claims at once, the others' stay */
    audio_owner_claim(&o, AUDIO_USER_STATION);
    audio_owner_claim(&o, AUDIO_USER_ZELLO);
    audio_owner_claim(&o, AUDIO_USER_ZELLO);
    audio_owner_release_all(&o, AUDIO_USER_STATION);
    CHECK(mode(&o) == AUDIO_PLAY_ON);
    audio_owner_release_all(&o, AUDIO_USER_ZELLO);
    CHECK(mode(&o) == AUDIO_PLAY_OFF);
    audio_owner_release(&o, AUDIO_USER_STATION);        /* the station's thread ending after the cut: nothing */
    CHECK(o.claims[AUDIO_USER_STATION] == 0);
}

/* The headphones standing in for the speaker: the receiver muted there from the first claim to the last, the
 * speaker's mode left alone; one on each */
static void test_headphones(void) {
    audio_owner_t o = {0};

    audio_owner_place(&o, AUDIO_USER_STATION, true);
    audio_owner_place(&o, AUDIO_USER_ZELLO, true);
    audio_owner_claim(&o, AUDIO_USER_STATION);
    CHECK(rx_mute(&o));
    CHECK(mode(&o) == AUDIO_PLAY_OFF);
    audio_owner_claim(&o, AUDIO_USER_ZELLO);
    audio_owner_release(&o, AUDIO_USER_STATION);        /* the first to end does not unmute under the other */
    CHECK(rx_mute(&o));
    audio_owner_release(&o, AUDIO_USER_ZELLO);
    CHECK(!rx_mute(&o));

    audio_owner_place(&o, AUDIO_USER_ZELLO, false);
    audio_owner_claim(&o, AUDIO_USER_STATION);
    audio_owner_claim(&o, AUDIO_USER_ZELLO);
    CHECK(rx_mute(&o) && mode(&o) == AUDIO_PLAY_ON);
    audio_owner_release(&o, AUDIO_USER_ZELLO);
    CHECK(rx_mute(&o) && mode(&o) == AUDIO_PLAY_OFF);
    audio_owner_release(&o, AUDIO_USER_STATION);
    CHECK(!rx_mute(&o) && mode(&o) == AUDIO_PLAY_OFF);
    /* The station on the headphones is held by a link's message as on the speaker */
    audio_owner_place(&o, AUDIO_USER_ZELLO, true);
    audio_owner_claim(&o, AUDIO_USER_STATION);
    CHECK(audio_owner_claim(&o, AUDIO_USER_ZELLO));
    CHECK(audio_owner_held(&o, AUDIO_USER_STATION));
}

/* A voice link over the station */
static void test_priority(void) {
    audio_owner_t o = {0};

    CHECK(audio_owner_is_link(AUDIO_USER_ZELLO) && audio_owner_is_link(AUDIO_USER_SVX));
    CHECK(!audio_owner_is_link(AUDIO_USER_STATION));

    /* No station: nothing to drop */
    CHECK(!audio_owner_claim(&o, AUDIO_USER_ZELLO));
    audio_owner_release(&o, AUDIO_USER_ZELLO);

    audio_owner_claim(&o, AUDIO_USER_STATION);
    CHECK(!audio_owner_held(&o, AUDIO_USER_STATION));
    CHECK(audio_owner_claim(&o, AUDIO_USER_ZELLO));     /* the message starts: the station's queue to drop, once */
    CHECK(audio_owner_held(&o, AUDIO_USER_STATION));
    CHECK(!audio_owner_held(&o, AUDIO_USER_ZELLO));
    CHECK(!audio_owner_claim(&o, AUDIO_USER_ZELLO));    /* a nested claim: dropped already */
    CHECK(!audio_owner_claim(&o, AUDIO_USER_SVX));      /* another link while one plays: dropped already */
    CHECK(!audio_owner_held(&o, AUDIO_USER_SVX));       /* links are not held by links */
    audio_owner_release(&o, AUDIO_USER_ZELLO);
    audio_owner_release(&o, AUDIO_USER_ZELLO);
    CHECK(audio_owner_held(&o, AUDIO_USER_STATION));    /* SVXConnect still has it */
    audio_owner_release(&o, AUDIO_USER_SVX);
    CHECK(!audio_owner_held(&o, AUDIO_USER_STATION));   /* the station goes on */
    CHECK(mode(&o) == AUDIO_PLAY_ON);

    /* A station started during a message: held from its first piece */
    audio_owner_release(&o, AUDIO_USER_STATION);
    audio_owner_claim(&o, AUDIO_USER_ZELLO);
    audio_owner_claim(&o, AUDIO_USER_STATION);
    CHECK(audio_owner_held(&o, AUDIO_USER_STATION));
    audio_owner_release(&o, AUDIO_USER_ZELLO);
    CHECK(!audio_owner_held(&o, AUDIO_USER_STATION));
    audio_owner_release(&o, AUDIO_USER_STATION);
}

/* A link recording the hand mic: Rec over Play, the station held as by a message */
static void test_rec(void) {
    audio_owner_t o = {0};

    audio_owner_claim(&o, AUDIO_USER_STATION);
    CHECK(audio_owner_rec(&o, AUDIO_USER_ZELLO, true));
    CHECK(mode(&o) == AUDIO_PLAY_VOICE_REC);
    CHECK(audio_owner_held(&o, AUDIO_USER_STATION));
    audio_owner_rec(&o, AUDIO_USER_ZELLO, false);
    CHECK(mode(&o) == AUDIO_PLAY_ON);
    CHECK(!audio_owner_held(&o, AUDIO_USER_STATION));
    audio_owner_release(&o, AUDIO_USER_STATION);
    CHECK(!audio_owner_rec(&o, AUDIO_USER_ZELLO, true));    /* no station */
    CHECK(mode(&o) == AUDIO_PLAY_VOICE_REC);
    audio_owner_release_all(&o, AUDIO_USER_ZELLO);          /* a key: its Rec goes too */
    CHECK(mode(&o) == AUDIO_PLAY_OFF);
}

/* Both writing in 20 ms ticks as audio.c lets them, the station all along, a Zello message from tick 10 to 19: what
 * reaches the speaker is the station, then the message alone, then the station; never both in a stretch */
static void test_timeline(void) {
    audio_owner_t o = {0};
    char          heard[64] = "";
    int           drops = 0;

    audio_owner_claim(&o, AUDIO_USER_STATION);
    for (int t = 0; t < 30; t++) {
        char tick[3] = "";

        if (t == 10 && audio_owner_claim(&o, AUDIO_USER_ZELLO)) drops++;
        if (t == 20) audio_owner_release(&o, AUDIO_USER_ZELLO);
        if (!audio_owner_held(&o, AUDIO_USER_STATION)) strcat(tick, "s");
        if (o.claims[AUDIO_USER_ZELLO]) strcat(tick, "z");
        CHECK(strlen(tick) == 1);
        strcat(heard, tick);
        CHECK(mode(&o) == AUDIO_PLAY_ON);
    }
    CHECK(drops == 1);
    CHECK(!strcmp(heard, "sssssssssszzzzzzzzzzssssssssss"));
}

int main(void) {
    test_idle();
    test_counting();
    test_headphones();
    test_priority();
    test_rec();
    test_timeline();
    printf("audio_owner: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
