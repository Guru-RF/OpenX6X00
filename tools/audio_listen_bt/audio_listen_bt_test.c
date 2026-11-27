/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the Internet radio's station on Bluetooth headphones (src/audio.c: audio_listen_open,
 *  audio_listen_play), #380: with a headset connected the station stopped with "the speaker took no audio", and
 *  played without it.
 *
 *  PulseAudio (16.1, as on the card images) is stood in for by a model of its playback stream: the buffer it gives
 *  a stream with PA_STREAM_ADJUST_LATENCY (protocol-native.c fix_playback_buffer_attr: tlength is the 800 ms asked
 *  for less the sink's latency, minreq 20 ms, prebuf tlength less minreq), the room the client sees (what the server
 *  requested), and the prebuf (pulsecore/memblockq.c: nothing is played, and nothing more is requested, until the
 *  queue holds prebuf bytes). A headset's sink has a fixed latency (module-bluez5-device.c: 25 ms and one encoder
 *  block, so it depends on the headset), which audio.c cannot choose. For some of those latencies the room left after
 *  whole 1024-sample pieces is less than one piece but more than the prebuf still lacks: the stream then never
 *  starts, its room never grows, and audio_listen_play gave up after its timeout. The speaker's sink honours the
 *  latency asked, and never got there. usleep() runs the test's clock. `make` in tools/audio_listen_bt.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "audio.h"
#include "audio_owner.h"

#include <pulse/pulseaudio.h>
#include <pulse/rtclock.h>

#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failed, checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            failed++;                                                                   \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                               \
    } while (0)

/* ---- the test's clock: usleep() moves it, get_time() reads it ---- */

static uint64_t now_us;

int __wrap_usleep(useconds_t us) {
    now_us += us ? us : 1;
    return 0;
}

uint64_t get_time(void) {
    return now_us / 1000;
}

/* ---- PulseAudio's playback stream, as 16.1 sizes and plays it ---- */

#define RATE        44100
#define FRAME       2                   /* mono S16 */
#define MAX_STREAMS 8

typedef struct {
    bool     used, playback;
    uint32_t tlength, minreq, prebuf;   /* bytes, as the server fixed them */
    int64_t  write_index, read_index;   /* the server's queue */
    int64_t  missing;                   /* memblockq's: played since its last request, to request again */
    bool     in_prebuf;
    size_t   requested;                 /* the client's room: pa_stream_writable_size() */
    uint64_t at_us;                     /* the sink played it up to then */
    int64_t  played;                    /* bytes the sink took from it */
} model_t;

static model_t  streams[MAX_STREAMS];
static uint64_t bt_latency_us;          /* the headset sink's fixed latency */
static bool     phones;

static size_t usec_to_bytes_round_up(uint64_t us) {
    return (size_t)((us * RATE + PA_USEC_PER_SEC - 1) / PA_USEC_PER_SEC) * FRAME;
}

static uint64_t bytes_to_usec(size_t b) {
    return (uint64_t)(b / FRAME) * PA_USEC_PER_SEC / RATE;
}

/* memblockq's pa_memblockq_pop_missing(): a request of at least minreq, or any while it prebuffers */
static void pop_missing(model_t *m) {
    bool prebuf_active = m->in_prebuf ? m->write_index - m->read_index < m->prebuf : m->read_index >= m->write_index;

    if (m->missing <= 0 || ((size_t)m->missing < m->minreq && !prebuf_active)) {
        return;
    }
    m->requested += (size_t)m->missing;
    m->missing = 0;
}

/* The sink takes the stream's audio at its rate up to now, once the prebuf is there; an underrun prebuffers again */
static void run_to_now(model_t *m) {
    uint64_t dt = now_us - m->at_us;

    m->at_us = now_us;
    if (m->in_prebuf) {
        if (m->write_index - m->read_index < m->prebuf) {
            return;     /* silence: the read index stays */
        }
        m->in_prebuf = false;
    }
    int64_t want = (int64_t)(dt * RATE / PA_USEC_PER_SEC) * FRAME;
    int64_t have = m->write_index - m->read_index;
    int64_t take = want < have ? want : have;

    m->read_index += take;
    m->missing += take;
    m->played += take;
    if (m->read_index >= m->write_index) {
        m->in_prebuf = true;
    }
    pop_missing(m);
}

pa_stream *pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map) {
    (void)c; (void)name; (void)ss; (void)map;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!streams[i].used) {
            memset(&streams[i], 0, sizeof(streams[i]));
            streams[i].used = true;
            return (pa_stream *)&streams[i];
        }
    }
    return NULL;
}

/* protocol-native.c fix_playback_buffer_attr() with ADJUST_LATENCY, the client asking tlength only */
int pa_stream_connect_playback(pa_stream *s, const char *dev, const pa_buffer_attr *attr, pa_stream_flags_t flags,
                               const pa_cvolume *volume, pa_stream *sync_stream) {
    model_t *m = (model_t *)s;
    (void)volume; (void)sync_stream;

    if (!(flags & PA_STREAM_ADJUST_LATENCY) || attr->minreq != (uint32_t)-1 || attr->prebuf != (uint32_t)-1) {
        return -1;      /* not what audio.c asks: the model does not cover it */
    }
    uint32_t tlength = attr->tlength;
    uint32_t process = (uint32_t)usec_to_bytes_round_up(20 * PA_USEC_PER_MSEC);
    uint32_t q = tlength / 4;

    q -= q % FRAME;
    m->minreq = process < q ? process : q;

    uint64_t tl_us   = bytes_to_usec(tlength);
    uint64_t mr_us   = bytes_to_usec(m->minreq);
    uint64_t sink_us = tl_us > 2 * mr_us ? (tl_us - 2 * mr_us) / 2 : 0;
    bool     bt      = dev && !strncmp(dev, "bluez_sink.", 11);
    uint64_t conf_us = bt ? bt_latency_us : sink_us;     /* a fixed latency, or the one asked for */
    uint64_t t_us    = tl_us >= conf_us ? tl_us - conf_us : tl_us;

    if (t_us < conf_us + 2 * mr_us) {
        t_us = conf_us + 2 * mr_us;
    }
    m->tlength     = usec_to_bytes_round_up(t_us) != usec_to_bytes_round_up(tl_us) ? usec_to_bytes_round_up(t_us)
                                                                                     : tlength;
    m->prebuf      = m->tlength + FRAME - m->minreq;
    m->playback    = true;
    m->in_prebuf   = true;
    m->missing     = m->tlength;
    m->at_us       = now_us;
    pop_missing(m);     /* the create reply's request: the whole tlength */
    return 0;
}

size_t pa_stream_writable_size(const pa_stream *s) {
    model_t *m = (model_t *)s;

    run_to_now(m);
    return m->requested;
}

int pa_stream_write(pa_stream *s, const void *data, size_t nbytes, pa_free_cb_t free_cb, int64_t offset,
                    pa_seek_mode_t seek) {
    model_t *m = (model_t *)s;
    (void)data; (void)free_cb; (void)offset; (void)seek;

    run_to_now(m);
    m->write_index += (int64_t)nbytes;     /* against what was requested (memblockq.c write_index_changed, account) */
    m->requested = nbytes < m->requested ? m->requested - nbytes : 0;
    return 0;
}

pa_operation *pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    model_t *m = (model_t *)s;
    static int op;
    (void)cb; (void)userdata;

    m->read_index = m->write_index;     /* the queue dropped, prebuffering again */
    m->in_prebuf = true;
    return (pa_operation *)&op;
}

int pa_stream_disconnect(pa_stream *s) {
    (void)s;
    return 0;
}

void pa_stream_unref(pa_stream *s) {
    ((model_t *)s)->used = false;
}

pa_stream_state_t pa_stream_get_state(const pa_stream *s) {
    (void)s;
    return PA_STREAM_READY;
}

uint32_t pa_stream_get_device_index(const pa_stream *s) {
    (void)s;
    return 7;
}

void pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    (void)s; (void)cb; (void)userdata;
}

void pa_stream_set_read_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata) {
    (void)s; (void)cb; (void)userdata;
}

int pa_stream_connect_record(pa_stream *s, const char *dev, const pa_buffer_attr *attr, pa_stream_flags_t flags) {
    (void)s; (void)dev; (void)attr; (void)flags;
    return 0;
}

int pa_stream_peek(pa_stream *s, const void **data, size_t *nbytes) {
    (void)s;
    *data = NULL;
    *nbytes = 0;
    return 0;
}

int pa_stream_drop(pa_stream *s) {
    (void)s;
    return 0;
}

static int fake_op;

pa_operation *pa_stream_drain(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    (void)s; (void)cb; (void)userdata;
    return (pa_operation *)&fake_op;
}

pa_operation_state_t pa_operation_get_state(const pa_operation *o) {
    (void)o;
    return PA_OPERATION_DONE;
}

void pa_operation_cancel(pa_operation *o) {
    (void)o;
}

void pa_operation_unref(pa_operation *o) {
    (void)o;
}

/* The receiver's loopback in the headphones muted while the station plays (audio.c loop_mute_set) */
static bool mute_last;

pa_operation *pa_context_get_sink_input_info_list(pa_context *c, pa_sink_input_info_cb_t cb, void *userdata) {
    (void)c; (void)cb;
    mute_last = ((uintptr_t)userdata & 1) != 0;
    return (pa_operation *)&fake_op;
}

pa_operation *pa_context_set_sink_input_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb,
                                             void *userdata) {
    (void)c; (void)idx; (void)mute; (void)cb; (void)userdata;
    return (pa_operation *)&fake_op;
}

pa_time_event *pa_context_rttime_new(const pa_context *c, pa_usec_t usec, pa_time_event_cb_t cb, void *userdata) {
    (void)c; (void)usec; (void)cb; (void)userdata;
    return NULL;
}

pa_usec_t pa_rtclock_now(void) {
    return now_us;
}

size_t pa_usec_to_bytes(pa_usec_t t, const pa_sample_spec *spec) {
    return (size_t)(t * spec->rate / PA_USEC_PER_SEC) * spec->channels * 2;
}

static int fake_mloop, fake_ctx;
static pa_mainloop_api fake_api;

pa_threaded_mainloop *pa_threaded_mainloop_new(void) {
    return (pa_threaded_mainloop *)&fake_mloop;
}

int pa_threaded_mainloop_start(pa_threaded_mainloop *m) {
    (void)m;
    return 0;
}

pa_mainloop_api *pa_threaded_mainloop_get_api(pa_threaded_mainloop *m) {
    (void)m;
    return &fake_api;
}

void pa_threaded_mainloop_lock(pa_threaded_mainloop *m) {
    (void)m;
}

void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m) {
    (void)m;
}

void pa_threaded_mainloop_wait(pa_threaded_mainloop *m) {
    (void)m;
}

void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int wait_for_accept) {
    (void)m; (void)wait_for_accept;
}

pa_context *pa_context_new(pa_mainloop_api *a, const char *name) {
    (void)a; (void)name;
    return (pa_context *)&fake_ctx;
}

void pa_context_set_state_callback(pa_context *c, pa_context_notify_cb_t cb, void *userdata) {
    (void)c; (void)cb; (void)userdata;
}

int pa_context_connect(pa_context *c, const char *server, pa_context_flags_t flags, const pa_spawn_api *api) {
    (void)c; (void)server; (void)flags; (void)api;
    return 0;
}

pa_context_state_t pa_context_get_state(const pa_context *c) {
    (void)c;
    return PA_CONTEXT_READY;
}

void pa_context_unref(pa_context *c) {
    (void)c;
}

int pa_context_errno(const pa_context *c) {
    (void)c;
    return 0;
}

const char *pa_strerror(int error) {
    (void)error;
    return "test";
}

/* ---- the GUI's side ---- */

/* btaudio-ctl's /tmp/btaudio-hp with Mute speaker on: the headset's A2DP sink */
bool btaudio_hp_sink(char *buf, size_t size) {
    if (!phones) return false;
    snprintf(buf, size, "bluez_sink.00_11_22_33_44_55.a2dp_sink");
    return true;
}

void dsp_put_audio_samples(size_t n, int16_t *samples) {
    (void)n; (void)samples;
}

void radio_set_play_mode(audio_play_mode_t mode) {
    (void)mode;
}

void _lv_log_add(int8_t level, const char *file, int line, const char *func, const char *format, ...) {
    (void)level; (void)file; (void)line; (void)func; (void)format;
}

static const char *running = "";

static void watchdog(int sig) {
    (void)sig;
    static const char msg[] = "audio_listen_bt_test: hung in ";

    write(2, msg, sizeof(msg) - 1);
    write(2, running, strlen(running));
    write(2, "\n", 1);
    _exit(1);
}

static bool go_on(void) {
    return true;
}

/* dialog_iradio.c's play thread: its 4096-byte reads (2048 samples) handed on as they come, STALL_MS 3000. Returns
 * the first audio_listen_play() result other than 0, or 0 after `ms` of audio; *start_ms when the sink took its
 * first sample (or -1) */
static int station(uint64_t ms, int *start_ms) {
    static int16_t buf[2048];
    uint64_t       t0 = now_us;
    int            res = 0;
    model_t       *m;

    CHECK(audio_listen_open(AUDIO_USER_STATION) == (phones ? 1 : 0));
    m = NULL;
    for (int i = MAX_STREAMS - 1; i >= 0 && !m; i--) {
        if (streams[i].used && streams[i].playback && streams[i].write_index == 0) {
            m = &streams[i];    /* the station's: opened last, nothing in it yet */
        }
    }
    CHECK(m != NULL);
    *start_ms = -1;
    for (uint64_t fed = 0; fed < ms * RATE / 1000 && m; fed += 2048) {
        res = audio_listen_play(AUDIO_USER_STATION, buf, 2048, go_on, 3000);
        run_to_now(m);
        if (*start_ms < 0 && m->played > 0) {
            *start_ms = (int)((now_us - t0) / 1000);
        }
        if (res != 0) {
            break;
        }
    }
    audio_listen_end(AUDIO_USER_STATION);
    return res;
}

/* Every fixed latency a headset's A2DP sink may have, 25 to 100 ms in 0.1 ms steps: the station starts within a
 * second and plays 5 s on each, where 97 of these 751 stopped it with "the speaker took no audio" */
static void test_headset_latencies(void) {
    int stalled = 0, late = 0, n = 0, first_bad = -1;

    phones = true;
    for (uint64_t lat = 25000; lat <= 100000; lat += 100, n++) {
        int start_ms;

        bt_latency_us = lat;
        running = "the station on a headset";
        alarm(5);
        int res = station(5000, &start_ms);
        alarm(0);

        if (res != 0) {
            stalled++;
            if (first_bad < 0) first_bad = (int)(lat / 100);
        } else if (start_ms < 0 || start_ms > 1000) {
            late++;
        }
        CHECK(!mute_last);      /* the receiver back in the headphones after each */
    }
    if (stalled || late) {
        fprintf(stderr, "headset sink latencies: %d of %d stopped the station, %d started late; first at %d.%d ms\n",
                stalled, n, late, first_bad / 10, first_bad % 10);
    }
    CHECK(stalled == 0);
    CHECK(late == 0);
}

/* The one that shows it: a sink latency of 35 ms (25 ms and a 10 ms block), the stream's tlength 765 ms */
static void test_headset_35ms(void) {
    int start_ms;

    phones = true;
    bt_latency_us = 35000;
    running = "the station on a 35 ms headset";
    alarm(5);
    CHECK(station(5000, &start_ms) == 0);
    alarm(0);
    CHECK(start_ms >= 0 && start_ms < 1000);
}

/* The speaker, as without a headset: its sink honours the latency asked */
static void test_speaker(void) {
    int start_ms;

    phones = false;
    running = "the station on the speaker";
    alarm(5);
    CHECK(station(5000, &start_ms) == 0);
    alarm(0);
    CHECK(start_ms >= 0 && start_ms < 1000);
}

/* A headset whose sink takes nothing at all (the link stalled) still stops the station after 3 s, as before */
static void test_headset_dead(void) {
    static int16_t buf[2048];
    model_t       *m = NULL;

    phones = true;
    bt_latency_us = 30000;
    CHECK(audio_listen_open(AUDIO_USER_STATION) == 1);
    for (int i = MAX_STREAMS - 1; i >= 0 && !m; i--) {
        if (streams[i].used && streams[i].playback && streams[i].write_index == 0) m = &streams[i];
    }
    CHECK(m != NULL);
    if (m) {
        m->prebuf = UINT32_MAX;     /* never starts, whatever is written */
    }
    running = "the station on a dead headset";
    alarm(5);
    int res = 0;

    for (int i = 0; i < 100 && res == 0; i++) {
        res = audio_listen_play(AUDIO_USER_STATION, buf, 2048, go_on, 3000);
    }
    alarm(0);
    CHECK(res == -2);
    audio_listen_end(AUDIO_USER_STATION);
    CHECK(!mute_last);
}

int main(void) {
    signal(SIGALRM, watchdog);
    audio_init();
    test_speaker();
    test_headset_35ms();
    test_headset_latencies();
    test_headset_dead();
    if (failed) {
        fprintf(stderr, "audio_listen_bt_test: %d of %d checks failed\n", failed, checks);
        return 1;
    }
    printf("audio_listen_bt_test: %d checks passed\n", checks);
    return 0;
}
