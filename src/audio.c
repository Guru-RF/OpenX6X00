/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */
#define _GNU_SOURCE

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <math.h>

#include <pulse/pulseaudio.h>
#include <alsa/asoundlib.h>
#include <alsa/mixer.h>

#include "lvgl/lvgl.h"
#include "board.h"
#include "audio.h"
#include "meter.h"
#include "dsp.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "btaudio.h"

#define AUDIO_RATE_MS   100
#define PA_CONNECT_TRIES 20     /* a second apart: PulseAudio still coming up, or x6100_daemon restarting it */

static pa_threaded_mainloop *mloop;
static pa_mainloop_api      *mlapi;
static pa_context           *ctx;

static pa_stream            *play_stm;
static char                 *play_device = BOARD_PA_SINK;

/* Local playback on the Bluetooth headphones (audio_play_bt_begin), per
 * thread: the TX players never see it, so TX audio always goes to the codec. */
static __thread pa_stream   *bt_stm = NULL;

static pa_stream            *capture_stm;
static char                 *capture_device = BOARD_PA_SOURCE;

static pa_stream            *monitor_stm = NULL;

static float                peak_db = -60.0f;
static bool                 connected;      /* the context was READY once */

static void record_monitor_setup();

/* On the mainloop thread, with its lock held */
static void on_state_change(pa_context *c, void *userdata) {
    pa_context_state_t st = pa_context_get_state(c);

    if (connected && (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED)) {
        /* PulseAudio went away under us: every stream is dead, the receiver is silent and the
         * waterfall stands. Out, so x6100_daemon brings PulseAudio and us back. */
        LV_LOG_ERROR("PulseAudio gone (%s): exiting for a restart", pa_strerror(pa_context_errno(c)));
        fflush(stdout);
        fflush(stderr);
        _exit(AUDIO_EXIT_NO_SERVER);
    }
    pa_threaded_mainloop_signal(mloop, 0);
}

/* Connects ctx, trying PA_CONNECT_TRIES times a second apart; exits when PulseAudio never answers.
 * The wait needs the mainloop lock: without it pa_threaded_mainloop_wait() returns at once, and a
 * context that failed (no PulseAudio) had the old loop spinning on one core for ever. */
static void context_connect(void) {
    pa_threaded_mainloop_lock(mloop);
    for (int attempt = 1;; attempt++) {
        pa_context_state_t st = PA_CONTEXT_FAILED;

        ctx = pa_context_new(mlapi, "X6100 GUI");
        pa_context_set_state_callback(ctx, on_state_change, NULL);
        if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) >= 0) {
            while ((st = pa_context_get_state(ctx)) != PA_CONTEXT_READY && st != PA_CONTEXT_FAILED &&
                   st != PA_CONTEXT_TERMINATED) {
                pa_threaded_mainloop_wait(mloop);
            }
        }
        if (st == PA_CONTEXT_READY) {
            break;
        }
        LV_LOG_ERROR("PulseAudio: %s (try %d of %d)", pa_strerror(pa_context_errno(ctx)), attempt, PA_CONNECT_TRIES);
        pa_context_unref(ctx);
        ctx = NULL;
        if (attempt >= PA_CONNECT_TRIES) {
            pa_threaded_mainloop_unlock(mloop);
            LV_LOG_ERROR("No PulseAudio: exiting for a restart");
            exit(AUDIO_EXIT_NO_SERVER);
        }
        pa_threaded_mainloop_unlock(mloop);
        sleep(1);
        pa_threaded_mainloop_lock(mloop);
    }
    connected = true;
    pa_threaded_mainloop_unlock(mloop);
}

static void read_callback(pa_stream *s, size_t nbytes, void *udata) {
    int16_t *buf = NULL;

    pa_stream_peek(s, (const void**) &buf, &nbytes);
    dsp_put_audio_samples(nbytes / 2, buf);
    pa_stream_drop(s);
}

void audio_init() {
    audio_set_rec_vol(0.0f);
    audio_set_play_vol(0.0f);

    mloop = pa_threaded_mainloop_new();
    pa_threaded_mainloop_start(mloop);

    mlapi = pa_threaded_mainloop_get_api(mloop);
    context_connect();

    LV_LOG_USER("Connected");

    pa_buffer_attr  attr;

    pa_sample_spec  spec = {
        .format = PA_SAMPLE_S16NE,
        .channels = 1
    };

    memset(&attr, 0xff, sizeof(attr));
    int res;

    /* Play */

    spec.rate = AUDIO_PLAY_RATE,
    attr.fragsize = pa_usec_to_bytes(AUDIO_RATE_MS * PA_USEC_PER_MSEC, &spec);
    attr.tlength = attr.fragsize * 8;

    play_stm = pa_stream_new(ctx, "X6100 GUI Play", &spec, NULL);

    pa_threaded_mainloop_lock(mloop);
    /* DONT_MOVE: a Bluetooth device that connects must not take the radio audio. */
    res = pa_stream_connect_playback(play_stm, play_device, &attr, PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE, NULL, NULL);
    if (res < 0) {
        LV_LOG_ERROR("pa_stream_connect_playback() failed: %s", pa_strerror(pa_context_errno(ctx)));
    }
    pa_threaded_mainloop_unlock(mloop);

    /* Capture */

    spec.rate = AUDIO_CAPTURE_RATE,
    attr.fragsize = attr.tlength = pa_usec_to_bytes(AUDIO_RATE_MS * PA_USEC_PER_MSEC, &spec);

    capture_stm = pa_stream_new(ctx, "X6100 GUI Capture", &spec, NULL);

    pa_threaded_mainloop_lock(mloop);
    pa_stream_set_read_callback(capture_stm, read_callback, NULL);
    res = pa_stream_connect_record(capture_stm, capture_device, &attr, PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
    if (res < 0) {
        LV_LOG_ERROR("pa_stream_connect_record() failed: %s", pa_strerror(pa_context_errno(ctx)));
    }
    pa_threaded_mainloop_unlock(mloop);

    record_monitor_setup();
}

void audio_mixer_setup(x6100_base_ver_t base_ver) {
    int res;
    // overall level
    res = system("amixer sset 'Headphone',0 63,63"); // max output level
    // Play level from app to radio (for FT8)
    res = system("amixer sset 'AIF1 DA0',0 160,160");

    // capture audio from radio
    res = system("amixer sset 'Mic1',0 0,0 cap");
    // mic boost
    if (base_ver.rev >= 12) {
        // no need for mic boost on r12 and later
        res = system("amixer sset 'Mic1 Boost',0 0");
    } else {
        res = system("amixer sset 'Mic1 Boost',0 1");
    }
    // disable capturing from mixer
    res = system("amixer sset 'Mixer',0 nocap");
    res = system("amixer sset 'ADC Gain',0 3");
    res = system("amixer sset 'AIF1 AD0',0 160,160");
    res = system("amixer sset 'AIF1 AD0 Stereo',0 'Mix Mono'"); // Another option - Sum Mono
    res = system("amixer sset 'AIF1 Data Digital ADC',0 cap");
}

static pa_stream *cur_stm(void) {
    return bt_stm ? bt_stm : play_stm;
}

int audio_play(int16_t *samples_buf, size_t samples) {
    pa_stream *stm = cur_stm();

    while (true) {
        size_t size;

        pa_threaded_mainloop_lock(mloop);
        size = pa_stream_writable_size(stm);
        pa_threaded_mainloop_unlock(mloop);

        if (size >= (samples * 2)) {
            break;
        }

        usleep(1000);
    }

    pa_threaded_mainloop_lock(mloop);
    int res = pa_stream_write(stm, samples_buf, samples * 2, NULL, 0, PA_SEEK_RELATIVE);
    pa_threaded_mainloop_unlock(mloop);

    if (res < 0) {
        LV_LOG_ERROR("pa_stream_write() failed: %s", pa_strerror(pa_context_errno(ctx)));
    }

    return res;
}

/* Drops the queued audio, so a stop is immediate. */
void audio_play_flush() {
    pa_operation *op;

    pa_threaded_mainloop_lock(mloop);
    op = pa_stream_flush(cur_stm(), NULL, NULL);
    if (op) {
        pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(mloop);
}

void audio_play_wait() {
    pa_operation *op;
    int r;

    pa_threaded_mainloop_lock(mloop);
    op = pa_stream_drain(cur_stm(), NULL, NULL);
    pa_threaded_mainloop_unlock(mloop);

    if (!op) {
        return;     /* stream gone (headphones disconnected while playing) */
    }

    while (true) {
        pa_threaded_mainloop_lock(mloop);
        r = pa_operation_get_state(op);
        pa_threaded_mainloop_unlock(mloop);

        if (r == PA_OPERATION_DONE || r == PA_OPERATION_CANCELLED) {
            break;
        }

        usleep(1000);
    }

    pa_operation_unref(op);
}

static void on_stream_state(pa_stream *s, void *userdata) {
    pa_threaded_mainloop_signal(mloop, 0);
}

/* Local playback (voice message preview, recordings, alerts) into the
 * Bluetooth headphones while they replace the speaker, as SP2KMO's
 * audio_create_player_on() / bt_audio_playback_begin() in openx6100.
 * True: audio_play() from this thread goes to the headphones until
 * audio_play_bt_end(), and the caller must not set AUDIO_PLAY_ON.
 * False: no headphones, play on the speaker as before.
 * DONT_MOVE: if the headphones go away the stream ends, it never lands
 * on the codec (the modulator input). */
/* RX quiet in the headphones while a local sound plays there, as upstream does (SP2KMO): the
 * btaudio-ctl loopback ("Loopback from ...") into the same sink is muted, then unmuted.
 * ponytail: one local playback at a time; a GUI crash mid-play leaves it muted until btaudio-ctl
 * loads the loopback again (headphones reconnect) */
static struct { uint32_t sink, self; int mute; } loop_mute_req;

static void loop_mute_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *u) {
    (void)u;
    if (eol || !i || i->sink != loop_mute_req.sink || i->index == loop_mute_req.self ||
        !i->name || strncmp(i->name, "Loopback", 8)) {
        return;
    }
    pa_operation_unref(pa_context_set_sink_input_mute(c, i->index, loop_mute_req.mute, NULL, NULL));
}

/* With the mainloop locked */
static void loop_mute(bool mute) {
    loop_mute_req.sink = pa_stream_get_device_index(bt_stm);
    loop_mute_req.self = pa_stream_get_index(bt_stm);
    loop_mute_req.mute = mute;
    pa_operation_unref(pa_context_get_sink_input_info_list(ctx, loop_mute_cb, NULL));
}

/* A playback stream on sink, ready, or NULL. With the mainloop locked */
static pa_stream *stream_open(const char *name, const char *sink, const pa_buffer_attr *attr) {
    pa_sample_spec    spec = { .format = PA_SAMPLE_S16NE, .rate = AUDIO_PLAY_RATE, .channels = 1 };
    pa_stream_state_t st = PA_STREAM_FAILED;
    pa_stream         *s = pa_stream_new(ctx, name, &spec, NULL);

    pa_stream_set_state_callback(s, on_stream_state, NULL);
    if (pa_stream_connect_playback(s, sink, attr, PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE, NULL, NULL) == 0) {
        while ((st = pa_stream_get_state(s)) == PA_STREAM_CREATING) {
            pa_threaded_mainloop_wait(mloop);
        }
    }
    pa_stream_set_state_callback(s, NULL, NULL);
    if (st != PA_STREAM_READY) {
        LV_LOG_ERROR("Play on %s failed", sink);
        pa_stream_disconnect(s);
        pa_stream_unref(s);
        s = NULL;
    }
    return s;
}

/* Audio from a network client (the TCI server) into the codec, from the playing thread as audio_play_bt_begin:
 * 100 ms queued at most and playing from 60 ms, where play_stm queues 800 ms */
bool audio_play_lowlat_begin(void) {
    pa_sample_spec spec = { .format = PA_SAMPLE_S16NE, .rate = AUDIO_PLAY_RATE, .channels = 1 };
    pa_buffer_attr attr;

    if (bt_stm) {
        return false;
    }
    memset(&attr, 0xff, sizeof(attr));
    attr.tlength = pa_usec_to_bytes(100 * PA_USEC_PER_MSEC, &spec);
    attr.prebuf  = pa_usec_to_bytes(60 * PA_USEC_PER_MSEC, &spec);
    attr.minreq  = pa_usec_to_bytes(10 * PA_USEC_PER_MSEC, &spec);

    pa_threaded_mainloop_lock(mloop);
    bt_stm = stream_open("X6100 GUI Network voice", play_device, &attr);
    pa_threaded_mainloop_unlock(mloop);
    return bt_stm != NULL;
}

void audio_play_lowlat_end(void) {
    if (!bt_stm) {
        return;
    }
    pa_threaded_mainloop_lock(mloop);
    pa_stream_disconnect(bt_stm);
    pa_stream_unref(bt_stm);
    pa_threaded_mainloop_unlock(mloop);
    bt_stm = NULL;
}

/* audio_play without waiting: writes what fits and drops the rest, false when nothing fitted.
 * Not all or nothing: the server may cut the buffer to the prebuf size, and a stream that never
 * has room for a whole block never reaches its prebuf and never starts */
bool audio_play_if_room(int16_t *samples_buf, size_t samples) {
    pa_threaded_mainloop_lock(mloop);
    size_t bytes = LV_MIN(pa_stream_writable_size(cur_stm()), samples * 2) & ~(size_t)1;
    if (bytes) {
        pa_stream_write(cur_stm(), samples_buf, bytes, NULL, 0, PA_SEEK_RELATIVE);
    }
    pa_threaded_mainloop_unlock(mloop);
    return bytes > 0;
}

bool audio_play_bt_begin(void) {
    char              sink[128];
    pa_sample_spec    spec = { .format = PA_SAMPLE_S16NE, .rate = AUDIO_PLAY_RATE, .channels = 1 };
    pa_buffer_attr    attr;
    pa_stream         *s;

    if (bt_stm || !btaudio_hp_sink(sink, sizeof(sink))) {
        return false;
    }
    memset(&attr, 0xff, sizeof(attr));
    attr.tlength = pa_usec_to_bytes(AUDIO_RATE_MS * PA_USEC_PER_MSEC, &spec) * 8;

    pa_threaded_mainloop_lock(mloop);
    s = stream_open("X6100 GUI Local", sink, &attr);
    bt_stm = s;
    if (s) {
        loop_mute(true);
    }
    pa_threaded_mainloop_unlock(mloop);

    return s != NULL;
}

/* After audio_play_wait() / audio_play_flush() */
void audio_play_bt_end(void) {
    if (!bt_stm) {
        return;
    }
    pa_threaded_mainloop_lock(mloop);
    loop_mute(false);
    pa_stream_disconnect(bt_stm);
    pa_stream_unref(bt_stm);
    pa_threaded_mainloop_unlock(mloop);
    bt_stm = NULL;
}

void audio_gain_db(int16_t *buf, size_t samples, float gain, int16_t *out) {
    float scale = exp10f(gain / 20.0f);

    for (uint16_t i = 0; i < samples; i++) {
        int32_t x = buf[i] * scale;

        if (x > 32767) {
            x = 32767;
        }

        if (x < -32767) {
            x = -32767;
        }

        out[i] = x;
    }
}

void audio_gain_db_transition(int16_t *buf, size_t samples, float gain1, float gain2, int16_t *out) {
    float scale1 = exp10f(gain1 / 20.0f);
    float scale2 = exp10f(gain2 / 20.0f);
    float scale;
    for (uint16_t i = 0; i < samples; i++) {
        scale = scale1 + i * (scale2 - scale1) / samples;
        int32_t x = buf[i] * scale;

        if (x > 32767) {
            x = 32767;
        }

        if (x < -32767) {
            x = -32767;
        }

        out[i] = x;
    }
}

void audio_set_play_mode(audio_play_mode_t mode) {
    switch (mode)
    {
    case AUDIO_PLAY_OFF:
        x6100_control_record_set(false);
        x6100_control_hmic_set(param_i_get(cfg_hmic));
        x6100_control_imic_set(param_i_get(cfg_imic));
        break;

    case AUDIO_PLAY_ON:
        x6100_control_hmic_set(0);
    case AUDIO_PLAY_VOICE_REC:
        x6100_control_imic_set(0);
        x6100_control_record_set(true);
        break;

    default:
        break;
    }
}



float audio_set_play_vol(float db) {
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;
    const char *card = "default";
    const char *selem_name = "AIF1 DA0";

    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, card);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

    snd_mixer_selem_set_playback_dB_all(elem, (long)(db * 100.0f), 0);
    long db_long;
    snd_mixer_selem_get_playback_dB(elem, SND_MIXER_SCHN_MONO, &db_long);
    snd_mixer_close(handle);
    return (float)db_long / 100.0f;
}



float audio_set_rec_vol(float db) {
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;
    const char *card = "default";
    const char *selem_name = "ADC";

    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, card);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

    snd_mixer_selem_set_capture_dB_all(elem, (long)(db * 100.0f), 0);
    long db_long;
    snd_mixer_selem_get_capture_dB(elem, SND_MIXER_SCHN_MONO, &db_long);
    snd_mixer_close(handle);
    return (float) db_long / 100.0f;
}

float audio_get_peak_db() {
    return peak_db;
}

/* PulseAudio does the peak detection: float32 peaks, AUDIO_RATE_MS a second, instead of
 * scanning the 44.1 kHz S16 samples here. From Dyuldin's upstream 592aadcc */
static void monitor_cb(pa_stream *stream, size_t nbytes, void *udata) {
    const void *data;

    if (pa_stream_peek(stream, &data, &nbytes) < 0) {
        return;
    }
    if (!data || nbytes < sizeof(float)) {
        /* No data (a hole, or the stream is corked) */
        if (nbytes) {
            pa_stream_drop(stream);
        }
        return;
    }
    float peak = *(const float *)data;
    pa_stream_drop(stream);

    /* -90 dB floor, as the S16 scan had */
    peak_db = 20.0f * log10f(LV_CLAMP(1.0f / ((1UL << 15) - 1), peak, 1.0f));
}


static void record_monitor_setup() {

    pa_sample_spec  spec = {
        .format = PA_SAMPLE_FLOAT32,
        .channels = 1,
        .rate = AUDIO_RATE_MS,
    };
    pa_buffer_attr  attr = {
        .maxlength = (uint32_t) -1,
        .fragsize = sizeof(float),      /* one peak per fragment */
    };

    monitor_stm = pa_stream_new(ctx, "X6100 GUI Monitor", &spec, NULL);

    pa_threaded_mainloop_lock(mloop);
    pa_stream_set_read_callback(monitor_stm, monitor_cb, NULL);
    pa_stream_connect_record(monitor_stm, capture_device, &attr,
                             PA_STREAM_PEAK_DETECT | PA_STREAM_DONT_MOVE | PA_STREAM_ADJUST_LATENCY);
    pa_threaded_mainloop_unlock(mloop);
}
