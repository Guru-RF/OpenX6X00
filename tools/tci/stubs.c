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
#include "cw_encoder.h"
#include "msg_box.h"
#include "radio.h"
#include "remote_sessions.h"
#include "scheduler.h"
#include "screen_light.h"
#include "screen_main.h"
#include "smeter.h"
#include "tci_ask.h"
#include "tci_server.h"
#include "tx_meters.h"

#include <math.h>
#include <pthread.h>
#include <pulse/simple.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- settings: a value behind each opaque pointer ---- */
struct ParamInt { int32_t v; };
struct ParamFloat { float v; };
struct ComputedParamInt { int32_t v; };

static struct ParamInt         rit, key_speed = {15};
static struct ParamFloat       pwr = {5.0f};
static struct ComputedParamInt freq = {7074000}, mode = {radio_mode_lsb}, agc = {radio_agc_fast}, flo = {300},
                               fhi = {2700};
ParamInt         *cfg_rit = &rit, *cfg_key_speed = &key_speed;
ParamFloat       *cfg_pwr = &pwr;
ComputedParamInt *cfg_fg_freq = &freq, *cfg_cur_mode = &mode, *cfg_cur_agc = &agc, *cfg_cur_filter_low = &flo,
                 *cfg_cur_filter_high = &fhi;

int32_t param_i_get(const ParamInt *p) { return p->v; }
/* KEY Speed keeps to its limits, as the settings' validator does */
void    param_i_set(ParamInt *p, int32_t v) {
    p->v = p == &key_speed ? (v < BOARD_KEY_SPEED_MIN ? BOARD_KEY_SPEED_MIN : v > BOARD_KEY_SPEED_MAX ? BOARD_KEY_SPEED_MAX : v) : v;
}
float   param_f_get(const ParamFloat *p) { return p->v; }
int32_t cparam_i_get(const ComputedParamInt *p) { return p->v; }
void    cparam_i_set(ComputedParamInt *p, int32_t v) { p->v = v; }
bool    cfg_is_valid_hw_freq(int32_t f) { return f >= 100000 && f <= 55000000; }
void    cfg_pwr_set(float w) { pwr.v = w; fprintf(stderr, "[stub] power %.1f W\n", w); }

/* ---- the radio's keyer (cw_encoder.c): each text it was given kept in stub_cw_text, a line each, and for
 * tci_client_test.py in the file TCI_TEST_KEYED; the radio on the air while it sends. It sends until stub_cw_end: all
 * of it (as the real keyer ends a text) or cut short (the radio's own stops); main() ends a text by itself after
 * 20 ms a character. With stub_tx_refused its first key down is refused, as radio_set_morse_key does where the radio
 * may not transmit: started and over at once, not all of it sent ---- */
bool               stub_tx_refused;
char               stub_cw_text[4096];
int                stub_cw_stops;
static atomic_uint cw_sends, cw_on, cw_all;
static uint64_t    cw_until;        /* main(): when the text on the keyer is all sent */
static pthread_mutex_t cw_mux = PTHREAD_MUTEX_INITIALIZER;

static uint64_t stub_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

unsigned cw_encoder_send(const char *text, bool beacon) {
    const char *path = getenv("TCI_TEST_KEYED");
    FILE       *f    = path ? fopen(path, "a") : NULL;
    unsigned    n;
    (void)beacon;

    pthread_mutex_lock(&cw_mux);
    n = atomic_fetch_add(&cw_sends, 1) + 1;
    snprintf(stub_cw_text + strlen(stub_cw_text), sizeof(stub_cw_text) - strlen(stub_cw_text), "%s\n", text);
    cw_until = stub_ms() + 20 * strlen(text);
    atomic_store(&cw_on, stub_tx_refused ? 0 : n);
    pthread_mutex_unlock(&cw_mux);
    if (f) {
        /* the speed steps as the letters TCI writes them, so the file is text */
        for (const char *p = text; *p; p++)
            fputc(*p == CW_ENCODER_FASTER ? '+' : *p == CW_ENCODER_SLOWER ? '-' : *p == CW_ENCODER_SPEED ? '=' : *p, f);
        fputc('\n', f);
        fclose(f);
    }
    fprintf(stderr, "[stub] keyer %u: %s\n", n, text);
    return n;
}
void cw_encoder_stop(void) {
    if (atomic_exchange(&cw_on, 0)) {
        stub_cw_stops++;
        fprintf(stderr, "[stub] keyer stopped\n");
    }
}
cw_encoder_state_t cw_encoder_state(void) { return atomic_load(&cw_on) ? CW_ENCODER_SEND : CW_ENCODER_IDLE; }
unsigned cw_encoder_sending(void) { return atomic_load(&cw_on); }
unsigned cw_encoder_done(void) { return atomic_load(&cw_all); }
/* The text on the keyer over: all of it sent, or cut short */
void stub_cw_end(bool all) {
    unsigned n = atomic_exchange(&cw_on, 0);
    if (n && all)
        atomic_store(&cw_all, n);
}

/* ---- the message box: the last note, and how many ---- */
char stub_said[128];
int  stub_said_n;
void msg_schedule_text_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(stub_said, sizeof(stub_said), fmt, ap);
    va_end(ap);
    stub_said_n++;
    fprintf(stderr, "[stub] on the screen: %s\n", stub_said);
}

/* ---- the radio. stub_tx_refused: a key does nothing, as radio.c's tx_refused() on a frequency
 * the radio may not transmit on (the key is taken, the radio stays on receive) ---- */
static bool ptt, modem;
radio_state_t radio_get_state(void) { return ptt || modem || atomic_load(&cw_on) ? RADIO_TX : RADIO_RX; }
bool radio_set_ptt(bool tx) {
    if (tx && stub_tx_refused) return false;
    ptt = tx;
    fprintf(stderr, "[stub] ptt %d\n", tx);
    return true;
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

/* ---- the radio's question before a client on the network transmits (tci_ask.c): what the server asked, kept here
 * for tci_own_test.c, and for tci_client_test.py in the file TCI_TEST_ASK ("ID<TAB>WHO" while it is open, gone when
 * it closes); main() answers it with what the test writes in TCI_TEST_ANSWER (once, always or no) ---- */
uint32_t stub_ask_id;           /* the question on the "screen", 0 none */
uint32_t stub_ask_last;         /* the last one shown */
char     stub_ask_who[128];
int      stub_ask_shown, stub_ask_why;
void tci_ask_show(uint32_t id, const char *who, int seconds) {
    const char *path = getenv("TCI_TEST_ASK");
    FILE       *f    = path ? fopen(path, "w") : NULL;

    stub_ask_id = stub_ask_last = id;
    snprintf(stub_ask_who, sizeof(stub_ask_who), "%s", who);
    stub_ask_shown++;
    fprintf(stderr, "[stub] asked: %s wants to transmit (%u, %d s)\n", who, id, seconds);
    if (f) {
        fprintf(f, "%u\t%s\n", id, who);
        fclose(f);
    }
}
void tci_ask_close(uint32_t id, int why) {
    const char *path = getenv("TCI_TEST_ASK");

    if (id == stub_ask_id) {
        stub_ask_id = 0;
        if (path)
            unlink(path);
    }
    stub_ask_why = why;
    fprintf(stderr, "[stub] question %u closed: %d\n", id, why);
}

/* ---- the main screen ---- */
bool backlight_tick(void) { return false; }
bool main_screen_freq_locked(void) { return false; }
bool main_screen_mode_locked(void) { return false; }
void main_screen_set_freq(uint64_t f) { freq.v = (int32_t)f; }

/* ---- the meters: an S-meter that breathes; in TX the 0 before the base's first reading (tx_info_clear) ---- */
int16_t meter_get_raw_db(void) {
    static int n;
    return (int16_t)(-100 + (n++ % 30));
}
void tx_info_get(float *p, float *s) { *p = 0.0f; *s = 0.0f; }
float tx_info_get_alc(bool *greyed) {
    if (greyed) *greyed = false;
    return 0.0f;
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
/* The operator at the "screen": the answer the test wrote, to the question open now. "off" and "on" set asking
 * itself, as Settings or the TCI page do; "forget ADDR" forgets one allowed always, as the lists do */
static void answer_poll(void) {
    const char *path = getenv("TCI_TEST_ANSWER");
    char        word[64] = "";
    FILE       *f        = path ? fopen(path, "r") : NULL;

    if (!f)
        return;
    if (!fgets(word, sizeof(word), f))
        word[0] = 0;
    fclose(f);
    unlink(path);
    word[strcspn(word, "\r\n")] = 0;
    fprintf(stderr, "[stub] the operator: %s\n", word);
    if (!strcmp(word, "off") || !strcmp(word, "on"))
        tci_server_set_tx_confirm(!strcmp(word, "on"));
    else if (!strncmp(word, "forget ", 7))
        tci_server_forget(word + 7);
    else if (stub_ask_id)
        tci_server_answer(stub_ask_id, !strcmp(word, "once") ? TCI_ASK_ONCE : !strcmp(word, "always") ? TCI_ASK_ALWAYS
                                                                                                    : TCI_ASK_NO);
}

int main(int argc, char **argv) {
    const char *port = getenv("TCI_TEST_PORT");
    (void)argc; (void)argv;
    /* The conf: enabled on the test port; nobody allowed always yet */
    FILE *f = fopen(TCI_CONF, "w");
    if (f) {
        fprintf(f, "enabled=1\nport=%s\n", port ? port : "50001");
        fclose(f);
    }
    unlink(TCI_ALLOWED);
    tci_server_init();
    if (!tci_server_get_enabled())
        tci_server_set_enabled(true);
    fprintf(stderr, "[stub] TCI server on port %d\n", tci_server_port());
    for (int n = 0;; n++) {
        usleep(50000);
        answer_poll();
        pthread_mutex_lock(&cw_mux);
        if (atomic_load(&cw_on) && stub_ms() > cw_until)
            stub_cw_end(true);
        pthread_mutex_unlock(&cw_mux);
        if (n % 20 == 0 && tci_server_clients())
            fprintf(stderr, "[stub] %d client(s), first %s\n", tci_server_clients(), tci_server_client());
    }
}
#endif
