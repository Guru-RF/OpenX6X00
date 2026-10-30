/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the Winlink modem bridge: the radio, the settings, the player and PulseAudio stood in by a few
 *  variables, so src/winlink.c runs on a PC and a modem (ardopcf, x6x00-mercury, winlink_proto_test.py) can
 *  connect to it. Two of it make two stations: what one transmits the other hears. Build with `make` here.
 *
 *    WL_SOCK      the modem socket (WINLINK_ARDOP_SOCK), default /tmp/winlink-test.sock
 *    WL_RIG_PORT  the rigctld's port, default 14532
 *    WL_PLAYED    each transmission is written here when it ends (int16, 44.1 kHz), and a line is added to
 *                 WL_PLAYED.log: "played <samples> stopped <0|1>"
 *    WL_AIR_IN    a UNIX datagram socket this station hears the other on; without it, a tone of WL_TONE Hz
 *                 (default 1000)
 *    WL_AIR_OUT   the other station's WL_AIR_IN: what this one transmits goes there, as the radio sends it
 *    WL_PWR_MAX   the top of PWR in W, default BOARD_PWR_MAX (the X6200's 8 W); 5 is the X6200 on its battery
 *    WL_PWR_XVTR  a transverter's TX power limit in force, W (#104), default none; give WL_PWR_MAX the same
 *    WL_APP       a file: while it is there, its text is the digital app open (FT8...), else none is
 *  The receiver has a little noise under what it hears. The player keeps time as the low-latency stream does
 *  (100 ms queued), keys the radio for the whole of it and unkeys once the last sample is out.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE

#include "audio.h"
#include "cfg/cfg_api.h"
#include "channel_list.h"
#include "msg.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_light.h"
#include "screen_main.h"
#include "stub_env.h"
#include "tx_player.h"
#include "winlink.h"

#include <liquid/liquid.h>

#include <math.h>
#include <pthread.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PLAY_RATE   44100
#define AIR_SIZE    (10 * PLAY_RATE)    /* what the other station sent, not yet heard */
#define NOISE       30.0f               /* the receiver's noise, rms: -61 dBFS */

static uint64_t now_us(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static void sleep_until(uint64_t us) {
    uint64_t now = now_us();

    if (us > now)
        usleep((useconds_t)(us - now));
}

/* ---- the environment ---- */

const char *stub_sock(void) {
    const char *s = getenv("WL_SOCK");
    return s && *s ? s : "/tmp/winlink-test.sock";
}

int stub_rig_port(void) {
    const char *s = getenv("WL_RIG_PORT");
    return s && *s ? atoi(s) : 14532;
}

/* ---- settings: a value behind each opaque pointer ---- */

struct ParamInt { int32_t v; };
struct ComputedParamInt { int32_t v; };
struct ParamFloat { float v; };

static struct ParamInt         xit, rit, tx_shift, split, tx_low = {100}, tx_high = {2900};
static struct ComputedParamInt freq = {14105000}, mode = {radio_mode_usb_dig}, low = {100}, high = {2900};
static struct ParamFloat       pwr = {5.0f};
ParamInt         *cfg_xit = &xit, *cfg_rit = &rit, *cfg_band_tx_shift = &tx_shift, *cfg_band_split = &split;
ParamInt         *cfg_tx_filter_low = &tx_low, *cfg_tx_filter_high = &tx_high;
ComputedParamInt *cfg_fg_freq = &freq, *cfg_cur_mode = &mode, *cfg_cur_filter_low = &low, *cfg_cur_filter_high = &high;
ParamFloat       *cfg_pwr = &pwr;

int32_t param_i_get(const ParamInt *p) { return p->v; }
int32_t cparam_i_get(const ComputedParamInt *p) { return p->v; }
float   param_f_get(const ParamFloat *p) { return p->v; }
void    cparam_i_set(ComputedParamInt *p, int32_t v) { p->v = v; }
bool    cfg_is_valid_hw_freq(int32_t f) { return f >= 100000 && f <= 55000000; }
float   cfg_pwr_max_get(void) { return getenv("WL_PWR_MAX") ? (float)atof(getenv("WL_PWR_MAX")) : BOARD_PWR_MAX; }
float   cfg_pwr_xvtr_get(void) { return getenv("WL_PWR_XVTR") ? (float)atof(getenv("WL_PWR_XVTR")) : 0.0f; }

/* ---- the radio, the screen, the scheduler (the work done at once: no LVGL thread here) ---- */

static atomic_bool keyed;

radio_state_t radio_get_state(void) { return atomic_load(&keyed) ? RADIO_TX : RADIO_RX; }
bool          radio_get_modem(void) { return atomic_load(&keyed); }
int32_t       radio_tx_freq(void) { return freq.v; }
unsigned      radio_swr_trips(void) { return 0; }
uint32_t      radio_caps(void) { return HW_CAP_TX_FILTER; }

const char *channels_mode_name(int m) { return m == radio_mode_usb_dig ? "USB-D" : "USB"; }

bool backlight_tick(void) { return true; }

/* WL_APP: the app open is the text of that file, none while there is no file. Asked from several threads */
const char *main_screen_digital_app(void) {
    static __thread char name[32];
    const char          *path = getenv("WL_APP");
    FILE                *f;
    size_t               n;

    if (!path || !*path || !(f = fopen(path, "r")))
        return NULL;
    n = fread(name, 1, sizeof(name) - 1, f);
    fclose(f);
    name[n] = 0;
    return n ? name : NULL;
}

bool main_screen_freq_locked(void) { return false; }
bool main_screen_mode_locked(void) { return false; }
void main_screen_set_freq(uint64_t f) { freq.v = (int32_t)f; fprintf(stderr, "[stub] freq %d\n", freq.v); }

bool scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size) {
    (void)arg_size;
    fn(arg);
    return true;
}

void msg_schedule_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "[msg ] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void audio_play_flush(void) {}
bool audio_play_lowlat_begin(void) { return true; }
void audio_play_lowlat_end(void) {}

/* ---- the air: what this station sends goes to WL_AIR_OUT, what comes in on WL_AIR_IN is heard ---- */

static int             air_out = -1;
static struct sockaddr_un air_out_addr = {.sun_family = AF_UNIX};
static pthread_mutex_t air_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t         air[AIR_SIZE];
static size_t          air_head, air_len;
static bool            air_in;              /* WL_AIR_IN given: the other station, not the tone */

static void air_send(const int16_t *s, size_t n) {
    if (air_out < 0)
        return;
    sendto(air_out, s, n * sizeof(int16_t), MSG_DONTWAIT, (struct sockaddr *)&air_out_addr, sizeof(air_out_addr));
}

static void *air_thread(void *arg) {
    int     fd = *(int *)arg;
    int16_t buf[4096];

    for (;;) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);

        if (len <= 0)
            continue;
        pthread_mutex_lock(&air_lock);
        for (size_t i = 0; i < (size_t)len / sizeof(int16_t) && air_len < AIR_SIZE; i++, air_len++)
            air[(air_head + air_len) % AIR_SIZE] = buf[i];
        pthread_mutex_unlock(&air_lock);
    }
    return NULL;
}

static void air_init(void) {
    const char *in = getenv("WL_AIR_IN"), *out = getenv("WL_AIR_OUT");

    if (out && *out && strlen(out) < sizeof(air_out_addr.sun_path)) {
        air_out = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        strcpy(air_out_addr.sun_path, out);
    }
    if (in && *in && strlen(in) < sizeof(air_out_addr.sun_path)) {
        static int         fd;
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        pthread_t          th;

        strcpy(a.sun_path, in);
        unlink(in);
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            perror("[stub] air in");
            exit(1);
        }
        air_in = true;
        pthread_create(&th, NULL, air_thread, &fd);
        pthread_detach(th);
    }
}

/* ---- PulseAudio: the record stream at the rate asked, on the wall clock ---- */

struct pa_simple {
    unsigned      rate;
    msresamp_rrrf rs;           /* 44100 -> rate */
    uint64_t      next_us;      /* when the next read is due */
    double        phase;        /* the tone's */
    float         out[4096];    /* resampled, not read yet */
    size_t        out_len;
};

static float noise(void) {
    float u1 = (rand() + 1.0f) / (RAND_MAX + 2.0f), u2 = (rand() + 1.0f) / (RAND_MAX + 2.0f);

    return NOISE * sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

pa_simple *pa_simple_new(const char *server, const char *name, pa_stream_direction_t dir, const char *dev,
                         const char *stream_name, const pa_sample_spec *ss, const void *map,
                         const pa_buffer_attr *attr, int *error) {
    pa_simple *s = calloc(1, sizeof(*s));

    (void)server; (void)dir; (void)dev; (void)map; (void)attr; (void)error;
    s->rate    = ss->rate;
    s->rs      = msresamp_rrrf_create((float)ss->rate / PLAY_RATE, 60.0f);
    s->next_us = now_us();
    fprintf(stderr, "[stub] %s/%s: recording at %u Hz, %u bytes a fragment\n", name, stream_name, ss->rate,
            attr ? attr->fragsize : 0);
    return s;
}

int pa_simple_read(pa_simple *s, void *data, size_t bytes, int *error) {
    size_t n    = bytes / sizeof(int16_t);
    double tone = getenv("WL_TONE") ? atof(getenv("WL_TONE")) : 1000.0;

    (void)error;
    s->next_us += n * 1000000ULL / s->rate;
    sleep_until(s->next_us);
    while (s->out_len < n) {
        float    in[441];
        unsigned ny = 0;

        pthread_mutex_lock(&air_lock);
        for (size_t i = 0; i < 441; i++) {
            float v = 0.0f;

            if (air_in) {
                if (air_len) {
                    v = air[air_head];
                    air_head = (air_head + 1) % AIR_SIZE;
                    air_len--;
                }
            } else {
                v = 8000.0f * (float)sin(s->phase);
                s->phase += 2.0 * M_PI * tone / PLAY_RATE;
            }
            in[i] = (v + noise()) / 32768.0f;
        }
        pthread_mutex_unlock(&air_lock);
        msresamp_rrrf_execute(s->rs, in, 441, s->out + s->out_len, &ny);
        s->out_len += ny;
    }
    for (size_t i = 0; i < n; i++)
        ((int16_t *)data)[i] = (int16_t)lrintf(fmaxf(-32767.0f, fminf(32767.0f, s->out[i] * 32768.0f)));
    memmove(s->out, s->out + n, (s->out_len - n) * sizeof(float));
    s->out_len -= n;
    return 0;
}

void pa_simple_free(pa_simple *s) {
    msresamp_rrrf_destroy(s->rs);
    free(s);
}

const char *pa_strerror(int error) {
    (void)error;
    return "stub";
}

/* ---- the player: keyed for the whole transmission, on the wall clock, 100 ms queued ---- */

float tx_player_base_gain_offset(void) { return 0.0f; }

static void played_write(const int16_t *s, size_t n, bool stopped) {
    const char *path = getenv("WL_PLAYED");
    char        tmp[4096], log[4096];
    FILE       *f;

    if (!path || !*path)
        return;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(log, sizeof(log), "%s.log", path);
    if ((f = fopen(tmp, "wb"))) {
        fwrite(s, sizeof(int16_t), n, f);
        fclose(f);
        rename(tmp, path);
    }
    if ((f = fopen(log, "a"))) {
        fprintf(f, "played %zu stopped %d\n", n, stopped);
        fclose(f);
    }
}

bool tx_player_stream(tx_fill_fn_t fill, void *fill_ctx, int32_t offset_hz, float base_gain, tx_abort_fn_t abort,
                      void *abort_ctx) {
    int16_t  block[2048];
    int16_t *all = NULL;
    size_t   played = 0, cap = 0;
    bool     stopped = false;
    uint64_t t0 = now_us();

    (void)offset_hz;
    (void)base_gain;
    atomic_store(&keyed, true);
    fprintf(stderr, "[stub] keyed\n");
    for (;;) {
        size_t n = fill(block, 2048, fill_ctx);

        if (n == 0)
            break;
        if ((abort && abort(abort_ctx)) || !atomic_load(&keyed)) {
            stopped = true;
            break;
        }
        if (played + n > cap) {
            cap = (played + n) * 2;
            all = realloc(all, cap * sizeof(int16_t));
        }
        memcpy(all + played, block, n * sizeof(int16_t));
        air_send(block, n);
        played += n;
        sleep_until(t0 + played * 1000000ULL / PLAY_RATE - 100000);
    }
    sleep_until(t0 + played * 1000000ULL / PLAY_RATE);     /* the queue plays out */
    atomic_store(&keyed, false);
    fprintf(stderr, "[stub] unkeyed: %zu samples, %.2f s%s\n", played, played / (double)PLAY_RATE,
            stopped ? ", stopped" : "");
    played_write(all, played, stopped);
    free(all);
    return !stopped;
}

int main(void) {
    setvbuf(stderr, NULL, _IOLBF, 0);
    air_init();
    winlink_init();
    for (;;)
        pause();
}
