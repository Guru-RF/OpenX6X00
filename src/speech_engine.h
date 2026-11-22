/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the speech synthesizer under the spoken feedback (voice.cpp): RHVoice, loaded when first asked for
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 *
 *  The GUI does not link RHVoice: its library (libRHVoice, the C interface of third-party/rhvoice/RHVoice.h) is
 *  opened with dlopen() the first time something is to be said or Settings > Voice asks whether there is speech. A
 *  card image without it (those up to v0.3.13, a radio updated in place) runs the same GUI and has no speech; with
 *  speech Off nothing is loaded at all. The synthesizer and its voices stay in memory from the first announcement.
 *
 *  No GUI in here: the host test (tools/speech) builds this file alone.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The library is on this system (loaded now if it was not): any thread. The answer is kept */
bool speech_engine_available(void);

typedef struct {
    const char *voice;          /* a voice of the image, "slt" */
    double      rate;           /* 1.0: the voice's own */
    double      pitch;
    double      volume;
    const char *sink;           /* a PulseAudio sink (the Bluetooth headphones'), NULL: the default output */
    /* Asked before the synthesis starts and at every 20 ms of audio handed over: false ends the announcement
     * there, what is still queued dropped. NULL: said to its end */
    bool      (*go_on)(void *arg);
    void       *arg;
    /* Called once the synthesizer is loaded and the announcement is to be made, before its synthesis starts (the
     * speaker claimed then: the base has switched to this app's audio by the time the first samples play, and the
     * second or so the very first announcement takes to load the voices is not spent with the receiver muted); NULL:
     * not told */
    void      (*starts)(void *arg);
    void       *starts_arg;
} speech_say_t;

typedef enum {
    SPEECH_SAID = 0,            /* to its end, and heard: the stream drained */
    SPEECH_CUT,                 /* go_on() said no */
    SPEECH_NO_ENGINE,           /* no library, or it found no voices */
    SPEECH_FAILED               /* the synthesis or the output failed */
} speech_result_t;

/* Says text (UTF-8) and returns when it is over. One announcement at a time, from one thread (voice.cpp's) */
speech_result_t speech_engine_say(const char *text, const speech_say_t *how);

/* The host tests (tools/speech): another library and data than the system's ("" or NULL: the system's), before the
 * first use; and the audio counted, not played, at once or (paced) at the pace it would have played */
void     speech_engine_test_paths(const char *lib, const char *data, const char *config);
void     speech_engine_test_mute(bool on, bool paced);
uint32_t speech_engine_test_samples(void);      /* handed over by the last announcement */
int      speech_engine_test_rate(void);         /* its sample rate */

#ifdef __cplusplus
}
#endif
