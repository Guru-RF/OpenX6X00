/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the speech synthesizer's loading and cutting (src/speech_engine.c)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 *
 *  Without arguments: a system without the library (a card image without speech) has no speech and says so at
 *  once, twice over. With SPEECH_LIB, SPEECH_DATA and SPEECH_CONFIG (a libRHVoice built for this PC, RHVoice's data/
 *  and config/ folders): each voice of the image says a line, at its rate and at a faster one, an announcement is cut
 *  after 100 ms of audio and one before it starts, and a voice the image has not is refused. Nothing plays: the
 *  samples are counted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speech_engine.h"

static int fails;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                \
        }                                                           \
    } while (0)

static int  asked;
static int  allow;      /* go_on() says yes this many times, -1: always */

static bool go_on(void *arg) {
    (void)arg;
    asked++;
    return allow < 0 || asked <= allow;
}

int main(void) {
    const char  *l = getenv("SPEECH_LIB"), *data = getenv("SPEECH_DATA"), *config = getenv("SPEECH_CONFIG");
    speech_say_t how = {.voice = "slt", .rate = 1.0, .pitch = 1.0, .volume = 1.0, .go_on = go_on};

    speech_engine_test_mute(true, false);
    if (!l || !l[0]) {
        speech_engine_test_paths("/nonexistent/libRHVoice.so.5", NULL, NULL);
        CHECK(!speech_engine_available());
        CHECK(!speech_engine_available());
        allow = -1;
        CHECK(speech_engine_say("Hello", &how) == SPEECH_NO_ENGINE);
        CHECK(asked == 0);
        printf("speech_engine: no library, no speech: %s\n", fails ? "FAILED" : "ok");
        return fails ? 1 : 0;
    }

    static const char *const voices[] = {"slt", "alan", "bdl", "clb"};
    const char              *line     = "14.074, U S B digital, VFO A, 20 meters";
    uint32_t                 full     = 0;

    speech_engine_test_paths(l, data, config);
    CHECK(speech_engine_available());
    for (size_t i = 0; i < sizeof(voices) / sizeof(voices[0]); i++) {
        how.voice = voices[i];
        allow     = -1;
        asked     = 0;
        CHECK(speech_engine_say(line, &how) == SPEECH_SAID);
        CHECK(speech_engine_test_rate() == 24000);
        /* a line of this length is between one and ten seconds of speech */
        CHECK(speech_engine_test_samples() > 24000 && speech_engine_test_samples() < 240000);
        /* asked before the start and at every 20 ms */
        CHECK(asked >= (int)(speech_engine_test_samples() / 480));
        printf("  %-5s %.2f s\n", voices[i], speech_engine_test_samples() / 24000.0);
        if (i == 0) full = speech_engine_test_samples();
    }

    /* faster: shorter */
    how.voice = "slt";
    how.rate  = 1.5;
    CHECK(speech_engine_say(line, &how) == SPEECH_SAID);
    CHECK(speech_engine_test_samples() < full * 9 / 10);
    how.rate = 1.0;

    /* cut after 100 ms: no more than that handed over, and the next announcement whole again */
    allow = 6;      /* the start and five pieces */
    asked = 0;
    CHECK(speech_engine_say(line, &how) == SPEECH_CUT);
    CHECK(speech_engine_test_samples() <= 5 * 480);
    allow = -1;
    CHECK(speech_engine_say(line, &how) == SPEECH_SAID);
    CHECK(speech_engine_test_samples() == full);

    /* cut before it starts: nothing synthesized */
    allow = 0;
    asked = 0;
    CHECK(speech_engine_say(line, &how) == SPEECH_CUT);
    CHECK(speech_engine_test_samples() == 0);

    /* a voice the image has not: refused, and the engine goes on */
    allow     = -1;
    how.voice = "no-such-voice";
    CHECK(speech_engine_say(line, &how) != SPEECH_SAID);
    how.voice = "slt";
    CHECK(speech_engine_say("Speech off", &how) == SPEECH_SAID);

    printf("speech_engine: RHVoice loaded, four voices, cut: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
