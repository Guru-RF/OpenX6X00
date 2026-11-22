/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the speech synthesizer under the spoken feedback (speech_engine.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 *
 *  RHVoice (LGPL-2.1-or-later, Olga Yakovleva and others) through its C interface. Its own audio library
 *  (libRHVoice_audio, GPL) is not used: the samples it hands over are played here, on a PulseAudio stream at the
 *  voice's rate (24 kHz), in pieces of 20 ms so that an announcement is cut within one of them.
 */

#include "speech_engine.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pulse/error.h>
#include <pulse/simple.h>

#include "RHVoice.h"

#define PIECE_MS  20
#define BUFFER_MS 100       /* queued in PulseAudio: what a cut still has to drop */

static pthread_mutex_t load_mux = PTHREAD_MUTEX_INITIALIZER;
static bool            load_tried;
static void           *lib;
static char            test_lib[256], test_data[256], test_config[256];
static bool            test_mute, test_pace;
static uint32_t        test_samples;

static RHVoice_tts_engine (*rhv_new_engine)(const RHVoice_init_params *);
static RHVoice_message (*rhv_new_message)(RHVoice_tts_engine, const char *, unsigned int, RHVoice_message_type,
                                          const RHVoice_synth_params *, void *);
static void (*rhv_delete_message)(RHVoice_message);
static int (*rhv_speak)(RHVoice_message);

static RHVoice_tts_engine engine;
static bool               engine_tried;

/* The announcement being said: one at a time */
static struct {
    const speech_say_t *how;
    pa_simple          *out;
    bool                cut, failed;
} now;
static int voice_rate = 24000;      /* as RHVoice last said (set_sample_rate_cb) */

void speech_engine_test_paths(const char *l, const char *data, const char *config) {
    snprintf(test_lib, sizeof(test_lib), "%s", l ? l : "");
    snprintf(test_data, sizeof(test_data), "%s", data ? data : "");
    snprintf(test_config, sizeof(test_config), "%s", config ? config : "");
}

void speech_engine_test_mute(bool on, bool paced) {
    test_mute = on;
    test_pace = paced;
}

uint32_t speech_engine_test_samples(void) {
    return test_samples;
}

int speech_engine_test_rate(void) {
    return voice_rate;
}

bool speech_engine_available(void) {
    /* The radio's image has libRHVoice.so.5 (its SCons build); the name without a version is there as well */
    static const char *const names[] = {"libRHVoice.so.5", "libRHVoice.so"};

    pthread_mutex_lock(&load_mux);
    if (!load_tried) {
        load_tried = true;
        if (test_lib[0]) {
            lib = dlopen(test_lib, RTLD_NOW | RTLD_LOCAL);
        } else {
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !lib; i++) {
                lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
            }
        }
        if (lib) {
            rhv_new_engine     = dlsym(lib, "RHVoice_new_tts_engine");
            rhv_new_message    = dlsym(lib, "RHVoice_new_message");
            rhv_delete_message = dlsym(lib, "RHVoice_delete_message");
            rhv_speak          = dlsym(lib, "RHVoice_speak");
            if (!rhv_new_engine || !rhv_new_message || !rhv_delete_message || !rhv_speak) {
                dlclose(lib);
                lib = NULL;
            }
        }
    }
    pthread_mutex_unlock(&load_mux);
    return lib != NULL;
}

static bool going(void) {
    if (!now.cut && now.how->go_on && !now.how->go_on(now.how->arg)) {
        now.cut = true;
    }
    return !now.cut;
}

/* The stream at the voice's rate: false when PulseAudio gives none */
static bool out_open(void) {
    if (test_mute || now.out) {
        return true;
    }

    pa_sample_spec spec = {.format = PA_SAMPLE_S16NE, .rate = (uint32_t)voice_rate, .channels = 1};
    pa_buffer_attr attr = {.maxlength = (uint32_t)-1,
                           .tlength   = (uint32_t)(voice_rate * BUFFER_MS / 1000 * (int)sizeof(int16_t)),
                           .prebuf    = (uint32_t)-1,
                           .minreq    = (uint32_t)-1,
                           .fragsize  = (uint32_t)-1};

    now.out = pa_simple_new(NULL, "OpenX6X00 Speech", PA_STREAM_PLAYBACK, now.how->sink, "Speech", &spec, NULL, &attr,
                            NULL);
    if (!now.out) {
        now.failed = true;
    }
    return now.out != NULL;
}

/* RHVoice, before the first samples and again should the rate change: 0 is an error to it */
static int set_sample_rate_cb(int rate, void *user) {
    (void)user;
    if (now.out && rate != voice_rate) {
        pa_simple_free(now.out);
        now.out = NULL;
    }
    voice_rate = rate;
    return rate > 0 && out_open();
}

/* RHVoice, with each stretch of speech it has made: 0 stops the synthesis */
static int play_speech_cb(const short *samples, unsigned int count, void *user) {
    unsigned int piece = (unsigned int)(voice_rate * PIECE_MS / 1000);

    (void)user;
    for (unsigned int at = 0; at < count; at += piece) {
        unsigned int n = count - at < piece ? count - at : piece;

        if (!going()) {
            return 0;
        }
        test_samples += n;
        if (test_mute) {
            if (test_pace) {
                usleep((useconds_t)(n * 1000000ull / (unsigned)voice_rate));
            }
            continue;
        }
        if (!out_open() || pa_simple_write(now.out, samples + at, n * sizeof(short), NULL) < 0) {
            now.failed = true;
            return 0;
        }
    }
    return 1;
}

static bool engine_start(void) {
    if (!engine_tried) {
        RHVoice_init_params p;

        engine_tried = true;
        memset(&p, 0, sizeof(p));
        p.data_path                 = test_data[0] ? test_data : NULL;      /* NULL: /usr/share/RHVoice */
        p.config_path               = test_config[0] ? test_config : NULL;  /* NULL: /etc/RHVoice */
        p.callbacks.set_sample_rate = set_sample_rate_cb;
        p.callbacks.play_speech     = play_speech_cb;
        engine                      = rhv_new_engine(&p);
    }
    return engine != NULL;
}

speech_result_t speech_engine_say(const char *text, const speech_say_t *how) {
    RHVoice_synth_params sp;
    RHVoice_message      msg;

    test_samples = 0;
    if (!text || !how || !speech_engine_available() || !engine_start()) {
        return SPEECH_NO_ENGINE;
    }
    memset(&now, 0, sizeof(now));
    now.how = how;
    if (!going()) {
        return SPEECH_CUT;
    }
    if (how->starts) {
        how->starts(how->starts_arg);
    }

    memset(&sp, 0, sizeof(sp));
    sp.voice_profile    = how->voice;
    sp.relative_rate    = how->rate;
    sp.relative_pitch   = how->pitch;
    sp.relative_volume  = how->volume;
    sp.punctuation_mode = RHVoice_punctuation_default;
    sp.capitals_mode    = RHVoice_capitals_default;

    msg = rhv_new_message(engine, text, (unsigned int)strlen(text), RHVoice_message_text, &sp, NULL);
    if (!msg) {
        return SPEECH_FAILED;       /* a voice the image does not have */
    }
    if (!rhv_speak(msg) && !now.cut) {
        now.failed = true;
    }
    rhv_delete_message(msg);

    if (now.out) {
        if (now.cut || now.failed) {
            pa_simple_flush(now.out, NULL);
        } else {
            pa_simple_drain(now.out, NULL);
        }
        pa_simple_free(now.out);
        now.out = NULL;
    }
    return now.cut ? SPEECH_CUT : now.failed ? SPEECH_FAILED : SPEECH_SAID;
}
