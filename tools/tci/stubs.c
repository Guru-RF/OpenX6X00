/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the TCI server: the radio, the settings, the scheduler, the meters, the audio
 *  and PulseAudio stood in by a few variables, so src/tci_server.c runs on a PC and
 *  tci_client_test.py can talk to it. Build and run with `make` here. tci_own_test.c links
 *  them too (TCI_STUBS_NO_MAIN), the scheduler's work held back until it runs it (stub_defer).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE

#include "audio.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "remote_sessions.h"
#include "scheduler.h"
#include "screen_light.h"
#include "screen_main.h"
#include "smeter.h"
#include "tci_server.h"
#include "tx_meters.h"

#include <math.h>
#include <pulse/simple.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- settings: a value behind each opaque pointer ---- */
struct ParamInt { int32_t v; };
struct ParamFloat { float v; };
struct ComputedParamInt { int32_t v; };

static struct ParamInt         rit;
static struct ParamFloat       pwr = {5.0f};
static struct ComputedParamInt freq = {7074000}, mode = {radio_mode_lsb}, agc = {radio_agc_fast}, flo = {300},
                               fhi = {2700};
ParamInt         *cfg_rit = &rit;
ParamFloat       *cfg_pwr = &pwr;
ComputedParamInt *cfg_fg_freq = &freq, *cfg_cur_mode = &mode, *cfg_cur_agc = &agc, *cfg_cur_filter_low = &flo,
                 *cfg_cur_filter_high = &fhi;

int32_t param_i_get(const ParamInt *p) { return p->v; }
void    param_i_set(ParamInt *p, int32_t v) { p->v = v; }
float   param_f_get(const ParamFloat *p) { return p->v; }
int32_t cparam_i_get(const ComputedParamInt *p) { return p->v; }
void    cparam_i_set(ComputedParamInt *p, int32_t v) { p->v = v; }
bool    cfg_is_valid_hw_freq(int32_t f) { return f >= 100000 && f <= 55000000; }
void    cfg_pwr_set(float w) { pwr.v = w; fprintf(stderr, "[stub] power %.1f W\n", w); }

/* ---- the radio. stub_tx_refused: a key does nothing, as radio.c's tx_refused() on a frequency
 * the radio may not transmit on (the key is taken, the radio stays on receive) ---- */
static bool ptt, modem;
bool stub_tx_refused;
radio_state_t radio_get_state(void) { return ptt || modem ? RADIO_TX : RADIO_RX; }
void radio_set_ptt(bool tx) {
    if (tx && stub_tx_refused) return;
    ptt = tx;
    fprintf(stderr, "[stub] ptt %d\n", tx);
}
void radio_set_modem(bool tx) {
    if (tx && stub_tx_refused) return;
    modem = tx;
    fprintf(stderr, "[stub] modem %d\n", tx);
}
bool radio_get_modem(void) { return modem; }
void radio_set_pwr(float d) { fprintf(stderr, "[stub] power %.1f W\n", d); }
void radio_set_tci_mute(bool on) { fprintf(stderr, "[stub] tci mute %d\n", on); }
uint32_t radio_caps(void) { return 0; }     /* an X6100: no SAM, no WFM */

/* ---- the scheduler: no LVGL thread here, the work is done at once; with stub_defer it waits,
 * in order, until stub_run runs it, as on an LVGL thread busy elsewhere ---- */
bool stub_defer;
static struct { scheduler_fn_t fn; void *arg; } held[256];
static size_t nheld;

bool scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size) {
    void *copy = arg_size ? malloc(arg_size) : NULL;
    if (copy) memcpy(copy, arg, arg_size);
    if (stub_defer) {
        if (nheld == sizeof(held) / sizeof(held[0])) {
            free(copy);
            return false;
        }
        held[nheld].fn    = fn;
        held[nheld++].arg = copy;
        return true;
    }
    fn(copy);
    free(copy);
    return true;
}

/* The first n jobs waiting (all of them: n < 0), in the order they came; how many ran */
int stub_run(int n) {
    int ran = 0;
    while (nheld && (n < 0 || ran < n)) {
        scheduler_fn_t fn  = held[0].fn;
        void          *arg = held[0].arg;
        memmove(held, held + 1, --nheld * sizeof(held[0]));
        fn(arg);
        free(arg);
        ran++;
    }
    return ran;
}
bool scheduler_put_urgent(scheduler_fn_t fn, void *arg, size_t arg_size) { return scheduler_put(fn, arg, arg_size); }

/* ---- the browser remote's sessions (Tailscale Funnel, #58): two paired browsers, one that may transmit and
 * one that may not; the first is revoked while the file TCI_TEST_REVOKED names exists. Funnel pauses after three
 * wrong keys here (the remote server's own rule is ten in ten minutes) ---- */
static int funnel_wrong;
bool remote_server_session_check(const char *token, int *role, bool *tx, char *name, size_t n) {
    const char *revoked = getenv("TCI_TEST_REVOKED");
    bool        op      = token && !strcmp(token, "0123456789abcdef0123456789abcdef") && !(revoked && !access(revoked, F_OK));
    bool        listen  = token && !strcmp(token, "11111111111111111111111111111111");

    if (!op && !listen)
        return false;
    *role = 1;
    *tx   = op;
    if (name && n)
        snprintf(name, n, "%s", op ? "Kitchen phone (away)" : "Listener (away)");
    return true;
}
void remote_server_funnel_refused(const char *addr) {
    funnel_wrong++;
    fprintf(stderr, "[stub] a wrong key over Funnel from %s (%d)\n", addr, funnel_wrong);
}
bool remote_server_funnel_paused(void) { return funnel_wrong >= 3; }

/* ---- the main screen ---- */
bool backlight_tick(void) { return false; }
bool main_screen_freq_locked(void) { return false; }
bool main_screen_mode_locked(void) { return false; }
void main_screen_set_freq(uint64_t f) { freq.v = (int32_t)f; }

/* ---- the meters: an S-meter that breathes, 50 W out ---- */
int16_t meter_get_raw_db(void) {
    static int n;
    return (int16_t)(-100 + (n++ % 30));
}
void tx_info_get(float *p, float *s) { *p = 5.0f; *s = 1.3f; }
bool tx_info_refresh(uint8_t *seen, float *alc, float *p, float *s) {
    (*seen)++;
    if (alc) *alc = 0.4f;
    if (p) *p = 5.0f;
    if (s) *s = 1.3f;
    return true;
}

/* ---- the audio out: what each transmission played, counted, and kept in the file
 * TCI_TEST_PLAYED (int16 at AUDIO_PLAY_RATE) for the client test to read ---- */
static size_t played;
static FILE  *played_f;
void audio_play_wait(void) {}
void audio_play_flush(void) {}
bool audio_play_lowlat_begin(void) {
    const char *path = getenv("TCI_TEST_PLAYED");
    played   = 0;
    played_f = path ? fopen(path, "wb") : NULL;
    return true;
}
void audio_play_lowlat_end(void) {
    if (played_f) {
        fclose(played_f);
        played_f = NULL;
    }
    fprintf(stderr, "[stub] played %zu samples\n", played);
}
bool audio_play_if_room(int16_t *buf, size_t n) {
    played += n;
    if (played_f) fwrite(buf, sizeof(*buf), n, played_f);
    return true;
}

/* ---- PulseAudio: a 1 kHz tone, paced like a real stream ---- */
struct pa_simple { uint32_t rate; double phase; };
pa_simple *pa_simple_new(const char *server, const char *name, pa_stream_direction_t dir, const char *dev,
                         const char *sn, const pa_sample_spec *ss, const void *map, const pa_buffer_attr *attr,
                         int *error) {
    (void)server; (void)name; (void)dir; (void)dev; (void)sn; (void)map; (void)attr; (void)error;
    pa_simple *s = calloc(1, sizeof(*s));
    s->rate = ss->rate;
    return s;
}
int pa_simple_read(pa_simple *s, void *data, size_t bytes, int *error) {
    int16_t *p = data;
    size_t   n = bytes / 2;
    (void)error;
    for (size_t i = 0; i < n; i++) {
        p[i] = (int16_t)(8000 * sin(s->phase));
        s->phase += 2 * M_PI * 1000 / s->rate;
    }
    usleep((useconds_t)(n * 1000000ULL / s->rate));
    return 0;
}
void pa_simple_free(pa_simple *s) { free(s); }

#ifndef TCI_STUBS_NO_MAIN
int main(int argc, char **argv) {
    const char *port = getenv("TCI_TEST_PORT");
    (void)argc; (void)argv;
    /* The conf: enabled on the test port */
    FILE *f = fopen(TCI_CONF, "w");
    if (f) {
        fprintf(f, "enabled=1\nport=%s\n", port ? port : "50001");
        fclose(f);
    }
    tci_server_init();
    if (!tci_server_get_enabled())
        tci_server_set_enabled(true);
    fprintf(stderr, "[stub] TCI server on port %d\n", tci_server_port());
    for (;;) {
        sleep(1);
        if (tci_server_clients())
            fprintf(stderr, "[stub] %d client(s), first %s\n", tci_server_clients(), tci_server_client());
    }
}
#endif
