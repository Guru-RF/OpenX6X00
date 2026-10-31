/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of TUNE for an external tuner (src/tune_tx.c, #85 #84); see the Makefile.
 *
 *  The base: two VFOs (frequency and mode), split, the TX power and the modem bit, which the radio_* below set as
 *  radio.c sets the real one (radio_set_tx_freq's split held off, radio_set_modem's move in split, set_txpwr's caps,
 *  radio_set_tune's guard and cap). The settings beside it are what the base must come back to, and nothing may
 *  write them. The radio thread's SWR protection runs as the audio goes out: every 10 ms of it on the air is a
 *  reading through tune_swr_limit and swr_guard_check as radio.c calls them, and a trip unkeys as swr_trip_cb does.
 *  The clock is the test's own, moved on by the audio played.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "button_bar.h"
#include "cfg/cfg_api.h"
#include "ext_tune.h"
#include "jtty_tx.h"
#include "msg_box.h"
#include "olivia_tx.h"
#include "params/params.h"
#include "psk_tx.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_main.h"
#include "swr_guard.h"
#include "tune_tx.h"
#include "tx_player.h"
#include "winlink.h"

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- The clock: the audio played moves it on ---------------------------------------------------------------- */

static _Atomic uint64_t virt_ns = 1000ULL * 1000000000ULL;

int __real_clock_gettime(clockid_t c, struct timespec *ts);
int __wrap_clock_gettime(clockid_t c, struct timespec *ts) {
    if (c != CLOCK_MONOTONIC) return __real_clock_gettime(c, ts);
    uint64_t ns = atomic_load(&virt_ns);
    ts->tv_sec  = ns / 1000000000ULL;
    ts->tv_nsec = ns % 1000000000ULL;
    return 0;
}

static uint64_t now_ms(void) {
    return atomic_load(&virt_ns) / 1000000ULL;
}

/* ---- The settings, and the base ----------------------------------------------------------------------------- */

static struct {
    int32_t freq[2];
    int     mode[2];
    int     cur;        /* the VFO listened on */
    bool    split;
    int32_t shift;      /* TX shift */
    float   pwr;
} cfg;

static struct {
    int32_t freq[2];
    int     mode[2];
    bool    split, modem;
    float   pwr;
    bool    atu;        /* its ATU in: radio.c sends the setting (cfg_atu_enabled) to it as it is set */
} base;

ParamFloat *cfg_pwr = (ParamFloat *)&cfg.pwr;

float param_f_get(const ParamFloat *p) {
    return *(const float *)p;
}

params_t   params = {.ft8_output_gain_offset = {-6.0f}, .play_gain_db_f = {0.0f}, .line_in = 10};
static int params_writes;

void params_float_set(params_float_t *p, float v) {
    p->x = v;
    params_writes++;
}

void msg_schedule_text_fmt(const char *fmt, ...) {
    va_list ap;

    printf("        screen: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static radio_state_t state = RADIO_RX;     /* RADIO_TX: something else is on the air */
static uint32_t      caps  = HW_CAP_TX_LEVEL;
static bool          split_held, digi_cap;
static float         tune_cap;
/* A transverter's TX power limit (#104): the radio's top (cfg_pwr_max_get) and set_txpwr's cap, 0 none */
static float         xvtr_w;
static tune_guard_t  guard;

/* What the scenario's antenna and base do, and what was seen on the air */
static struct {
    float    swr;           /* the SWR the base reads while it transmits */
    float    swr_limit;     /* Settings > TX Cal > SWR protection */
    float    over;          /* the power read: the base's power set times this */
    float    pwr_up_at_s;   /* PWR turned up this far into the tune (0 never), to pwr_up W */
    float    pwr_up;
    float    app_slot_at_s; /* an app's slot starts this far into the tune (0 never) */
    bool     app_slot_at_key;   /* or in the moment the tune has started and not keyed yet */
    bool     app_slot_at_end;   /* or in the moment its audio is over and it has not put the radio back yet */
    bool     tune_at_app_key;   /* a tune asked as an app's slot has started and not keyed yet */
    float    stop_at_s;     /* the operator stops it (0 never) */
    float    event_at_s;    /* event() this far into a tune of ext_tune.c's (the knob turned, a mode key, a stop) */
    void   (*event)(void);
} sc;

static struct {
    int      keys;          /* times keyed */
    bool     atu;           /* the base's ATU in at the key */
    int32_t  rf;            /* where the carrier went at the key: the VFO + the tone in USB */
    int      mode;          /* the base's mode at the key */
    bool     split;         /* its split bit at the key */
    float    max_pwr;       /* the most the base was set to while keyed */
    uint64_t key_ms, unkey_ms;
    int      trips;
    int      app_sent;      /* the app's slot: -1 not tried, 0 refused, 1 sent */
    int      app_sent_at_key;
    int      app_sent_at_end;
    int      tune_at_app_key;   /* how that tune ended, -1 not asked */
    int16_t  first[2048];   /* the first audio, and the last */
    size_t   nfirst;
    int16_t  last[2048];
    size_t   nlast;
    float    tune_w, tune_s;    /* tx_info_set_tune, as last told */
    float    len;           /* tx_info_set_length */
    float    asked;         /* the first power tx_player asked of the radio (radio_set_pwr), 0 none */
} seen;

static swr_guard_t swr_guard;
static bool        app_slot_done, app_key_done, app_end_done, tune_key_done, pwr_up_done, event_done;
static bool        in_app;     /* the scenario's own app slot is being sent */
static int16_t     slot[4410];

static int pwr_changes;      /* the base's power set to another value */

/* The tunes the app's slot sees asked (tune_at_app_key) are never on the air long */
static bool stop_now(void *ctx) {
    (void)ctx;
    return true;
}

/* An app's slot, on a thread of its own as the apps send theirs (the player's claim is the thread's): sent or not */
static void *slot_thread(void *arg) {
    *(int *)arg = tx_player_play(slot, 4410, 1500, tx_player_base_gain_offset(), NULL, NULL);
    return NULL;
}

static int app_slot(void) {
    pthread_t th;
    int       sent = -1;

    pthread_create(&th, NULL, slot_thread, &sent);
    pthread_join(th, NULL);
    return sent;
}

/* A TUNE asked meanwhile, on its own thread as ext_tune.c runs it: how it ended */
static void *tune_thread(void *arg) {
    tune_plan_t p;

    tune_plan(&p, cfg.mode[cfg.cur], 1500, 1.0f, 10.0f, 15);
    *(int *)arg = tune_tx_run(&p, stop_now, NULL);
    return NULL;
}

static void set_txpwr(float w) {
    if (digi_cap && tune_cap <= 0.0f && w > TX_PLAYER_MAX_PWR_W) w = TX_PLAYER_MAX_PWR_W;
    if (tune_cap > 0.0f && w > tune_cap) w = tune_cap;
    if (xvtr_w > 0.0f && w > xvtr_w) w = xvtr_w;
    pwr_changes += base.pwr != w;
    base.pwr = w;
    if (base.modem && w > seen.max_pwr) seen.max_pwr = w;
}

radio_state_t radio_get_state(void) {
    return state;
}

bool radio_get_modem(void) {
    return base.modem;
}

int32_t radio_tx_freq(void) {
    int32_t fg = cfg.freq[cfg.cur];

    if (!cfg.split) return fg;
    if (cfg.shift) return cfg.cur == 0 ? fg + cfg.shift : fg - cfg.shift;
    return cfg.freq[1 - cfg.cur];
}

void radio_set_tx_freq(int32_t f) {
    if (f == 0) {
        base.freq[0] = cfg.freq[0];
        base.freq[1] = cfg.freq[1];
        if (split_held) {
            split_held = false;
            base.split = cfg.split;
        }
        return;
    }
    if (cfg.split && !split_held) {
        split_held = true;
        base.split = false;
    }
    base.freq[cfg.cur] = f;
    /* tx_player moves the VFO just before it keys: an app's slot that starts now still finds the radio in RX */
    if (sc.app_slot_at_key && !app_key_done && tune_cap > 0.0f && !base.modem) {
        app_key_done = true;
        seen.app_sent_at_key = app_slot();
    }
    /* The other way round: the app's slot moved the VFO and is about to key, the radio still in RX, as a TUNE is
     * asked (its own thread on the radio: here tx_player sees the app's transmission under way all the same) */
    if (sc.tune_at_app_key && in_app && !tune_key_done && !base.modem) {
        pthread_t th;

        tune_key_done = true;
        pthread_create(&th, NULL, tune_thread, &seen.tune_at_app_key);
        pthread_join(th, NULL);
    }
}

void radio_set_modem(bool tx) {
    static bool moved;

    if (tx && cfg.split && !split_held) {
        radio_set_tx_freq(radio_tx_freq());
        moved = true;
    }
    if (tx && !base.modem) {
        int m = base.mode[cfg.cur];

        seen.keys++;
        seen.atu     = base.atu;
        seen.rf      = base.freq[cfg.cur] + (m == radio_mode_lsb || m == radio_mode_lsb_dig ? -1 : 1) * TX_PLAYER_AUDIO_HZ;
        seen.mode    = m;
        seen.split   = base.split;
        seen.key_ms  = now_ms();
        seen.max_pwr = base.pwr;
    }
    if (!tx && base.modem) seen.unkey_ms = now_ms();
    base.modem = tx;
    if (!tx && moved) {
        moved = false;
        radio_set_tx_freq(0);
    }
}

void radio_set_pwr(float w) {
    if (seen.asked == 0.0f) seen.asked = w;
    set_txpwr(w);
}

uint32_t radio_caps(void) {
    return caps;
}

void radio_set_tune(float w, uint32_t limit_ms) {
    /* The tune's audio is over and it puts the radio back: an app's slot that starts in this moment */
    if (w == 0.0f && sc.app_slot_at_end && !app_end_done && seen.keys > 0) {
        app_end_done = true;
        seen.app_sent_at_end = app_slot();
    }
    guard    = (tune_guard_t){.pwr_w = w, .start_ms = now_ms(), .limit_ms = limit_ms};
    tune_cap = w;
    set_txpwr(cfg.pwr);
}

void radio_set_tx_mode(int mode) {
    if (mode < 0) {
        base.mode[0] = cfg.mode[0];
        base.mode[1] = cfg.mode[1];
    } else {
        base.mode[cfg.cur] = mode;
    }
}

/* The settings' PWR turned up (the knob, the Panel): radio.c's on_pwr_change */
static void pwr_setting(float w) {
    cfg.pwr = w;
    set_txpwr(w);
}

/* ---- The air ------------------------------------------------------------------------------------------------ */

static uint8_t reading_id;
static float   reading_pwr;

/* One reading of the radio thread in TX: radio.c's SWR protection, a trip as swr_trip_cb */
static void radio_reading(void) {
    float pwr = base.pwr * sc.over;
    float limit;

    if (!base.modem) return;
    reading_pwr = pwr;
    reading_id++;
    limit = tune_swr_limit(&guard, pwr, sc.swr_limit, now_ms());
    if (swr_guard_check(&swr_guard, pwr, sc.swr, limit, now_ms())) {
        seen.trips++;
        msg_schedule_text_fmt("TX stopped: SWR %.1f", sc.swr);
        radio_set_modem(false);
    }
}

bool tx_info_refresh(uint8_t *s, float *alc, float *pwr, float *swr) {
    if (*s == reading_id) return false;
    *s = reading_id;
    if (alc) *alc = caps & HW_CAP_TX_NO_ALC ? 0.0f : 1.0f;     /* the ALC working a little: the level holds */
    if (pwr) *pwr = reading_pwr;
    if (swr) *swr = sc.swr;
    return true;
}

void tx_info_set_length(float s) {
    seen.len = s;
}

void tx_info_set_tune(float w, float s) {
    seen.tune_w = w;
    seen.tune_s = s;
}

float audio_set_play_vol(float db) {
    return db > 6.0f ? 6.0f : db < -30.0f ? -30.0f : db;
}

void audio_gain_db(int16_t *buf, size_t n, float g, int16_t *out) {
    float k = powf(10.0f, g / 20.0f);

    for (size_t i = 0; i < n; i++) out[i] = (int16_t)lrintf(buf[i] * k);
}

void audio_gain_db_transition(int16_t *buf, size_t n, float g1, float g2, int16_t *out) {
    for (size_t i = 0; i < n; i++)
        out[i] = (int16_t)lrintf(buf[i] * powf(10.0f, (g1 + (g2 - g1) * i / n) / 20.0f));
}

static uint64_t tune_t0;    /* the scenario's start */

/* The audio goes out in real time: the clock moves on by it, the radio reads every 10 ms */
int audio_play(int16_t *buf, size_t n) {
    static uint64_t carry_ns;

    if (seen.nfirst < 2048) {
        size_t k = n < 2048 - seen.nfirst ? n : 2048 - seen.nfirst;
        memcpy(seen.first + seen.nfirst, buf, k * sizeof(int16_t));
        seen.nfirst += k;
    }
    memmove(seen.last, seen.last + (n < 2048 ? n : 2048), (2048 - (n < 2048 ? n : 2048)) * sizeof(int16_t));
    memcpy(seen.last + 2048 - (n < 2048 ? n : 2048), buf + (n > 2048 ? n - 2048 : 0), (n < 2048 ? n : 2048) * sizeof(int16_t));
    seen.nlast = 2048;

    carry_ns += (uint64_t)n * 1000000000ULL / AUDIO_PLAY_RATE;
    while (carry_ns >= 10000000ULL) {
        carry_ns -= 10000000ULL;
        atomic_fetch_add(&virt_ns, 10000000ULL);
        radio_reading();
    }
    float t = (now_ms() - tune_t0) / 1000.0f;

    if (sc.pwr_up_at_s > 0.0f && !pwr_up_done && t >= sc.pwr_up_at_s) {
        pwr_up_done = true;
        pwr_setting(sc.pwr_up);
    }
    if (sc.app_slot_at_s > 0.0f && !app_slot_done && t >= sc.app_slot_at_s) {
        app_slot_done = true;
        seen.app_sent = app_slot();
    }
    if (sc.event && !event_done && t >= sc.event_at_s) {
        event_done = true;
        sc.event();
    }
    return (int)n;
}

void audio_play_wait(void) {
}

/* The operator's stop: TUNE again, at sc.stop_at_s */
static bool stop_cb(void *ctx) {
    (void)ctx;
    return sc.stop_at_s > 0.0f && (now_ms() - tune_t0) / 1000.0f >= sc.stop_at_s;
}

/* ---- The scenarios ------------------------------------------------------------------------------------------ */

static void reset(void) {
    memset(&sc, 0, sizeof(sc));
    memset(&seen, 0, sizeof(seen));
    sc.swr_limit = 2.0f;
    sc.over      = 1.0f;
    seen.app_sent = seen.app_sent_at_key = seen.app_sent_at_end = seen.tune_at_app_key = -1;
    app_slot_done = app_key_done = app_end_done = tune_key_done = pwr_up_done = event_done = false;
    memset(&swr_guard, 0, sizeof(swr_guard));
    atomic_fetch_add(&virt_ns, 5000000000ULL);  /* a new transmission: the guard's gap */
    base.freq[0] = cfg.freq[0];
    base.freq[1] = cfg.freq[1];
    base.mode[0] = cfg.mode[0];
    base.mode[1] = cfg.mode[1];
    base.split   = cfg.split;
    base.modem   = false;
    set_txpwr(cfg.pwr);
    params_writes = 0;
    pwr_changes   = 0;
    state = RADIO_RX;
}

/* The base as the settings say, everything the tune changed put back */
static bool as_set(void) {
    return base.freq[0] == cfg.freq[0] && base.freq[1] == cfg.freq[1] && base.mode[0] == cfg.mode[0] &&
           base.mode[1] == cfg.mode[1] && base.split == cfg.split && !base.modem && base.pwr == cfg.pwr &&
           !split_held && tune_cap == 0.0f && guard.pwr_w == 0.0f && seen.tune_w == 0.0f;
}

static const char *end_name(tune_end_t e) {
    static const char *const n[] = {"stopped", "its limit", "cut by the radio", "refused"};
    return n[e];
}

/* A tune as ext_tune.c makes it, from the settings and the open app's offset */
static tune_end_t run(int32_t app_hz, float pwr_w, int limit_s) {
    tune_plan_t p;

    tune_plan(&p, cfg.mode[cfg.cur], app_hz, pwr_w, 10.0f, limit_s);
    tune_t0 = now_ms();
    tune_end_t e = tune_tx_run(&p, stop_cb, NULL);
    printf("        tune %.1f W, %u s, tone at %+d Hz: %s after %.2f s on the air, %d key(s), carrier at %d Hz in mode %d\n",
           p.pwr_w, p.limit_ms / 1000, (int)p.offset_hz, end_name(e), (seen.unkey_ms - seen.key_ms) / 1000.0, seen.keys,
           (int)seen.rf, seen.mode);
    return e;
}

/* The tone: about TX_PLAYER_AUDIO_HZ (zero crossings over a whole block), faded in from 0 and out to 0 */
static bool tone_ok(void) {
    int    zc = 0;
    double hz;

    for (size_t i = 1; i < seen.nfirst; i++)
        if ((seen.first[i - 1] < 0) != (seen.first[i] < 0)) zc++;
    hz = zc / 2.0 * AUDIO_PLAY_RATE / seen.nfirst;
    return seen.nfirst == 2048 && fabs(hz - TX_PLAYER_AUDIO_HZ) < 40.0 && abs(seen.first[0]) < 200 &&
           abs(seen.last[2047]) < 200;
}

static void pure_rules(void) {
    tune_plan_t p;

    puts("-- the plan: the mode the base sends it in, the tone, the power, the limit");
    tune_plan(&p, radio_mode_lsb, 0, 1.0f, 10.0f, 15);
    check(p.tx_mode == radio_mode_usb_dig && p.offset_hz == 0 && p.pwr_w == 1.0f && p.limit_ms == 15000,
          "LSB, no app: USB-D for it, on the TX frequency, 1.0 W, 15 s");
    tune_plan(&p, radio_mode_usb_dig, 1500, 1.0f, 10.0f, 15);
    check(p.tx_mode == -1 && p.offset_hz == 1500, "FT8 (USB-D, TX offset 1500): the mode as it is, the tone at +1500 Hz");
    int modes[] = {radio_mode_usb, radio_mode_cw, radio_mode_cwr, radio_mode_am, radio_mode_nfm, radio_mode_lsb_dig,
                   radio_mode_sam, radio_mode_wfm};
    bool all = true;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        tune_plan(&p, modes[i], 0, 1.0f, 10.0f, 15);
        all = all && p.tx_mode == radio_mode_usb_dig;
    }
    check(all, "USB, CW, CW-R, AM, NFM, LSB-D, SAM, WFM: USB-D for the tune, the mode back after");
    tune_plan(&p, radio_mode_usb, 0, 12.0f, 12.0f, 15);
    check(p.pwr_w == TUNE_PWR_MAX_W && TUNE_PWR_MAX_W == BOARD_PWR_MAX,
          "12 W asked: %.1f W, the setting's top, the radio's PWR top (#87)", p.pwr_w);
    tune_plan(&p, radio_mode_usb, 0, 9.0f, BOARD_PWR_MAX, 15);
    check(p.pwr_w == (BOARD_PWR_MAX < 9.0f ? BOARD_PWR_MAX : 9.0f), "9 W asked, for a tuner that needs it (#87): %.1f W",
          p.pwr_w);
    tune_plan(&p, radio_mode_usb, 0, 8.0f, 5.0f, 15);
    check(p.pwr_w == 5.0f, "8 W asked with the radio's top at 5 W (the X6200's battery): %.1f W", p.pwr_w);
    tune_plan(&p, radio_mode_usb, 0, 4.0f, 2.5f, 15);
    check(p.pwr_w == 2.5f, "4 W asked with the radio's top at 2.5 W: %.1f W", p.pwr_w);
    tune_plan(&p, radio_mode_usb, 0, 0.0f, 10.0f, 2);
    check(p.pwr_w == TUNE_PWR_MIN_W && p.limit_ms == TUNE_TIME_MIN_S * 1000, "0 W and 2 s asked: %.1f W, %u s",
          p.pwr_w, p.limit_ms / 1000);
    tune_plan(&p, radio_mode_lsb, 1900, 1.0f, 10.0f, 15);
    check(p.tx_mode == radio_mode_usb_dig && p.offset_hz == -1900,
          "SSTV in LSB (40 m), its picture at 1900 Hz: USB-D for it, the tone 1900 Hz below the dial, where SSTV sends");
    tune_plan(&p, radio_mode_lsb_dig, 2125, 1.0f, 10.0f, 15);
    check(p.tx_mode == radio_mode_usb_dig && p.offset_hz == -2125, "RTTY in LSB-D at 2125 Hz: 2125 Hz below the dial");
    tune_plan(&p, radio_mode_usb, -300, 1.0f, 10.0f, 120);
    check(p.offset_hz == 0 && p.limit_ms == TUNE_TIME_MAX_S * 1000, "an offset below 0 and 120 s: on the TX frequency, %u s",
          p.limit_ms / 1000);

    puts("-- the radio's own ATU: off for a tune above 1 W (#87)");
    static const struct {
        float asked, top;
        bool  off;
    } atu[] = {{0.1f, 10.0f, false}, {0.5f, 10.0f, false}, {1.0f, 10.0f, false}, {1.1f, 10.0f, true},
               {5.0f, 10.0f, true},  {9.0f, BOARD_PWR_MAX, true}, {5.0f, 0.5f, false}, {5.0f, 1.0f, false}};
    all = true;
    for (size_t i = 0; i < sizeof(atu) / sizeof(atu[0]); i++) {
        tune_plan(&p, radio_mode_usb, 0, atu[i].asked, atu[i].top, 15);
        printf("        %.1f W asked, the radio's top %.1f W: %.1f W, ATU %s\n", atu[i].asked, atu[i].top, p.pwr_w,
               tune_atu_off(&p) ? "off" : "as it is");
        all = all && tune_atu_off(&p) == atu[i].off;
    }
    check(all, "0.1, 0.5 and 1.0 W: the ATU as it is; 1.1, 5 and 9 W: off; 5 W held to 0.5 or 1 W (a transverter's "
          "limit): as it is");

    puts("-- the refusals, in the order they are told");
    check(tune_refusal(&(tune_ask_t){.tx_freq_ok = true}) == TUNE_GO, "a frequency it transmits on, in RX: it tunes");
    check(tune_refusal(&(tune_ask_t){.tx_freq_ok = false, .on_air = true, .winlink = true}) == TUNE_NO_TX,
          "receive only (the band, split's other VFO, a TX shift): no TX on that frequency, first");
    check(tune_refusal(&(tune_ask_t){.tx_freq_ok = true, .on_air = true, .winlink = true}) == TUNE_ON_AIR,
          "on the air (an app, PTT, TCI, the ATU): on the air");
    check(tune_refusal(&(tune_ask_t){.tx_freq_ok = true, .winlink = true}) == TUNE_WINLINK,
          "a Winlink connection under way: Winlink holds the radio");

    puts("-- the SWR protection during a tune: off for the tune's own power and time only");
    tune_guard_t g = {.pwr_w = 1.0f, .start_ms = 100000, .limit_ms = 15000};
    check(tune_swr_limit(&g, 1.0f, 2.0f, 100500) == 0.0f, "1.0 W of a 1 W tune, 0.5 s in: off");
    check(tune_swr_limit(&g, 2.2f, 2.0f, 101000) == 0.0f, "2.2 W (twice and 0.2 W: the base's start): off");
    check(tune_swr_limit(&g, 2.3f, 2.0f, 101000) == 2.0f, "2.3 W: the setting, 2.0");
    check(tune_swr_limit(&g, 1.0f, 2.0f, 100000 + 15000 + TUNE_GRACE_MS) == 0.0f, "at the limit and the grace: off");
    check(tune_swr_limit(&g, 1.0f, 2.0f, 100000 + 15000 + TUNE_GRACE_MS + 1) == 2.0f, "past it: the setting");
    check(tune_swr_limit(&g, 1.0f, 2.0f, 99990) == 0.0f, "a reading from just before the tune's clock: off");
    check(tune_swr_limit(&(tune_guard_t){0}, 1.0f, 2.0f, 100500) == 2.0f, "no tune: the setting");
    check(tune_swr_limit(&(tune_guard_t){0}, 1.0f, 1.0f, 100500) == 1.0f, "no tune, protection Off: Off");

    puts("-- the same through the protection itself, readings every 10 ms at SWR 3.4");
    swr_guard_t sg = {0};
    uint64_t    t  = 200000, trip = 0;
    g = (tune_guard_t){.pwr_w = 1.0f, .start_ms = t, .limit_ms = 30000};
    for (uint64_t ms = 0; ms <= 30000 && !trip; ms += 10)
        if (swr_guard_check(&sg, 1.0f, 3.4f, tune_swr_limit(&g, 1.0f, 2.0f, t + ms), t + ms)) trip = ms;
    check(!trip, "a 30 s tune at 1 W: not stopped");
    g.pwr_w = 0.0f;     /* over: radio_set_tune(0) */
    for (uint64_t ms = 30010; ms <= 32000 && !trip; ms += 10)
        if (swr_guard_check(&sg, 1.0f, 3.4f, tune_swr_limit(&g, 1.0f, 2.0f, t + ms), t + ms)) trip = ms - 30000;
    check(trip >= SWR_GUARD_MS && trip <= SWR_GUARD_MS + 20, "PTT after it, the same antenna: stopped after %llu ms",
          (unsigned long long)trip);
    sg = (swr_guard_t){0};
    trip = 0;
    g = (tune_guard_t){.pwr_w = 1.0f, .start_ms = t + 40000, .limit_ms = 15000};
    for (uint64_t ms = 0; ms <= 3000 && !trip; ms += 10)
        if (swr_guard_check(&sg, 5.0f, 3.4f, tune_swr_limit(&g, 5.0f, 2.0f, t + 40000 + ms), t + 40000 + ms)) trip = ms;
    check(trip >= SWR_GUARD_MS && trip <= SWR_GUARD_MS + 20, "5 W read during a 1 W tune: stopped after %llu ms",
          (unsigned long long)trip);
}

static void transmissions(void) {
    tune_end_t e;

    puts("-- SSB: LSB on 7.150 MHz, PWR 5 W, no app; stopped after 3 s");
    cfg = (typeof(cfg)){.freq = {7150000, 7100000}, .mode = {radio_mode_lsb, radio_mode_lsb}, .pwr = 5.0f};
    reset();
    sc.stop_at_s = 3.0f;
    e = run(0, 1.0f, 15);
    check(e == TUNE_END_STOP && seen.keys == 1, "ended by the stop, keyed once");
    check(seen.rf == 7150000 && seen.mode == radio_mode_usb_dig, "the carrier on 7.150.000 MHz, the base in USB-D for it");
    check(seen.max_pwr == 1.0f, "on the air at the tune's 1.0 W, not PWR's 5 W (%.1f W)", seen.max_pwr);
    check(as_set(), "after: VFO A 7.150.000 LSB, PWR 5.0 W, nothing of the tune left");
    check(params_writes == 0 && params.ft8_output_gain_offset.x == -6.0f, "the level the apps share untouched (%d writes)",
          params_writes);
    check(tone_ok(), "the tone: 1325 Hz, faded in from 0 and out to 0");

    puts("-- FT8: USB-D on 14.074 MHz, TX offset 1500 Hz, the tuner at SWR 3.4 all along, protection 2.0");
    cfg = (typeof(cfg)){.freq = {14074000, 14074000}, .mode = {radio_mode_usb_dig, radio_mode_usb_dig}, .pwr = 5.0f};
    reset();
    digi_cap = true;
    sc.swr = 3.4f;
    sc.app_slot_at_s   = 2.0f;
    sc.app_slot_at_key = true;
    e = run(1500, 1.0f, 5);
    check(e == TUNE_END_LIMIT && seen.trips == 0, "ran to its 5 s limit, the SWR protection aside (%d trips)", seen.trips);
    check(seen.unkey_ms - seen.key_ms >= 5000 && seen.unkey_ms - seen.key_ms < 5200, "on the air %.2f s",
          (seen.unkey_ms - seen.key_ms) / 1000.0);
    check(seen.rf == 14075500 && seen.mode == radio_mode_usb_dig, "the carrier on 14.075.500, where FT8 transmits");
    check(seen.app_sent == 0 && seen.keys == 1, "FT8's slot starting meanwhile: not sent, no second key");
    check(seen.app_sent_at_key == 0, "nor one that starts as the tune does, before it keys (one transmission at a time)");
    check(as_set(), "after: 14.074.000 USB-D, PWR 5.0 W");

    puts("-- FT8's slot in the moment the tune's audio is over, before the tune has put the radio back");
    reset();
    sc.app_slot_at_end = true;
    sc.stop_at_s       = 1.0f;
    e = run(1500, 1.0f, 15);
    check(e == TUNE_END_STOP && seen.app_sent_at_end == 0 && seen.keys == 1,
          "not sent (it went out at the tune's power, the SWR protection aside), keyed once (%d)", seen.keys);
    check(as_set(), "after: all put back");

    puts("-- a tune asked as WSPR's beacon at its own 0.2 W has started and not keyed yet");
    reset();
    sc.tune_at_app_key = true;
    tx_player_set_pwr(0.2f);
    in_app = true;
    bool beacon = tx_player_play(slot, 4410, 1500, tx_player_base_gain_offset(), NULL, NULL);
    in_app = false;
    tx_player_set_pwr(0.0f);
    check(seen.tune_at_app_key == TUNE_END_REFUSED && beacon && seen.keys == 1,
          "the tune refused, the beacon sent, keyed once (%d)", seen.keys);
    check(seen.max_pwr == 0.2f, "the beacon at its own 0.2 W, not PWR's 5 W put back by the tune (%.1f W)", seen.max_pwr);
    check(as_set(), "after: PWR 5.0 W, nothing of the tune left");

    puts("-- FT8 open (the apps' 7 W cap), TUNE at the radio's PWR top (#87)");
    reset();
    sc.stop_at_s = 1.0f;
    e = run(1500, BOARD_PWR_MAX, 15);
    check(e == TUNE_END_STOP && seen.asked == BOARD_PWR_MAX && seen.max_pwr == BOARD_PWR_MAX,
          "on the air at %.1f W, asked %.1f W: over the apps' 7 W, the tune's own", seen.max_pwr, seen.asked);
    check(as_set(), "after: PWR 5.0 W");
    cfg.pwr = 10.0f;
    reset();
    sc.stop_at_s = 1.0f;
    e = run(1500, 1.0f, 15);
    check(e == TUNE_END_STOP && seen.max_pwr == 1.0f, "PWR 10 W, a 1 W tune: 1.0 W (%.1f W)", seen.max_pwr);
    check(base.pwr == TX_PLAYER_MAX_PWR_W, "after it the base back at the apps' 7 W, not PWR's 10 W (%.1f W)", base.pwr);
    reset();
    bool sent = app_slot() == 1;
    check(sent && seen.max_pwr == TX_PLAYER_MAX_PWR_W, "an FT8 slot then, PWR 10 W: %.1f W, the cap as before",
          seen.max_pwr);
    cfg.pwr = 5.0f;
    digi_cap = false;

    puts("-- the same antenna, a base that reads 2.5 times the power set (the tune's own power exceeded)");
    reset();
    sc.swr  = 3.4f;
    sc.over = 2.5f;
    e = run(1500, 1.0f, 15);
    check(e == TUNE_END_CUT && seen.trips == 1, "stopped by the SWR protection");
    check(seen.unkey_ms - seen.key_ms >= SWR_GUARD_MS && seen.unkey_ms - seen.key_ms < SWR_GUARD_MS + 100,
          "after %llu ms, as any transmission", (unsigned long long)(seen.unkey_ms - seen.key_ms));
    check(as_set(), "after: all put back");

    puts("-- SSTV in LSB on 7.171 MHz, its picture at 1900 Hz (an app that does not move the VFO for its audio)");
    cfg = (typeof(cfg)){.freq = {7171000, 7171000}, .mode = {radio_mode_lsb, radio_mode_lsb}, .pwr = 5.0f};
    reset();
    sc.stop_at_s = 1.0f;
    e = run(1900, 1.0f, 15);
    check(e == TUNE_END_STOP && seen.rf == 7169100 && seen.mode == radio_mode_usb_dig,
          "the carrier on 7.169.100, where SSTV's picture goes out, not 3.8 kHz above it on 7.172.900 (%d)", (int)seen.rf);
    check(as_set(), "after: 7.171.000 LSB again");
    cfg = (typeof(cfg)){.freq = {14074000, 14074000}, .mode = {radio_mode_usb_dig, radio_mode_usb_dig}, .pwr = 5.0f};

    puts("-- PWR turned up to 10 W during a 1 W tune");
    reset();
    sc.pwr_up_at_s = 1.0f;
    sc.pwr_up      = 10.0f;
    sc.stop_at_s   = 2.0f;
    e = run(0, 1.0f, 15);
    check(e == TUNE_END_STOP && seen.max_pwr == 1.0f, "the base stayed at 1.0 W (%.1f W)", seen.max_pwr);
    check(as_set() && base.pwr == 10.0f, "after: the new PWR, 10.0 W");
    cfg.pwr = 5.0f;

    puts("-- PWR 0.5 W (a driven amplifier), the tune at its own 1 W");
    cfg.pwr = 0.5f;
    reset();
    sc.stop_at_s = 1.0f;
    e = run(0, 1.0f, 15);
    check(seen.max_pwr == 1.0f && as_set() && base.pwr == 0.5f, "1.0 W on the air, 0.5 W again after");
    cfg.pwr = 5.0f;

    puts("-- split with a -600 kHz TX shift on 10 m FM, VFO A 29.620 NFM");
    cfg = (typeof(cfg)){.freq = {29620000, 29020000}, .mode = {radio_mode_nfm, radio_mode_nfm}, .split = true,
                        .shift = -600000, .pwr = 5.0f};
    reset();
    sc.stop_at_s = 2.0f;
    e = run(0, 1.0f, 15);
    check(e == TUNE_END_STOP && seen.rf == 29020000 && !seen.split, "on the TX shift's 29.020.000, split held off for it");
    check(as_set(), "after: A 29.620.000 NFM, split on again");

    puts("-- the X6200: the ALC does not follow the drive, the power holds the setting");
    cfg = (typeof(cfg)){.freq = {21074000, 21074000}, .mode = {radio_mode_usb_dig, radio_mode_usb_dig}, .pwr = 5.0f};
    caps = HW_CAP_TX_LEVEL | HW_CAP_TX_NO_ALC;
    reset();
    sc.swr = 3.4f;
    e = run(1200, 1.0f, 5);
    check(e == TUNE_END_LIMIT && seen.trips == 0 && seen.max_pwr == 1.0f && as_set(), "ran its 5 s at 1.0 W, put back");
    check(params_writes == 0, "the level the apps share untouched");
    caps = HW_CAP_TX_LEVEL;

    puts("-- a 2 m transverter on 28.500 with a TX power limit of 0.5 W (#104): the radio's top 0.5 W, PWR down to it");
    cfg = (typeof(cfg)){.freq = {28500000, 28500000}, .mode = {radio_mode_usb, radio_mode_usb}, .pwr = 0.5f};
    xvtr_w = 0.5f;
    reset();
    sc.stop_at_s = 1.0f;
    tune_plan_t xp;
    tune_plan(&xp, cfg.mode[cfg.cur], 0, 1.0f, cfg_pwr_max_get(), 15);
    tune_t0 = now_ms();
    e = tune_tx_run(&xp, stop_cb, NULL);
    check(e == TUNE_END_STOP && xp.pwr_w == 0.5f && seen.max_pwr == 0.5f && as_set(),
          "TUNE at its 1.0 W: planned at 0.5 W, 0.5 W on the air (%.1f W), put back", seen.max_pwr);
    cfg.pwr = 0.2f;     /* turned down on the transverter: WSPR's own power is not PWR */
    reset();
    tx_player_set_pwr(5.0f);
    bool xsent = tx_player_play(slot, 4410, 1500, tx_player_base_gain_offset(), NULL, NULL);
    tx_player_set_pwr(0.0f);
    check(xsent && seen.asked == 0.5f && seen.max_pwr == 0.5f && as_set(),
          "PWR 0.2 W, WSPR's own 5 W beacon: 0.5 W asked (%.1f W), 0.5 W on the air (%.1f W), PWR's 0.2 W after",
          seen.asked, seen.max_pwr);
    cfg.pwr = 5.0f;     /* the base's limit first, PWR comes down after it on the LVGL thread */
    reset();
    xsent = app_slot() == 1;
    check(xsent && seen.asked == 0.5f && seen.max_pwr == 0.5f,
          "an app's slot with PWR still at 5 W: 0.5 W asked (%.1f W), 0.5 W on the air (%.1f W)", seen.asked,
          seen.max_pwr);
    xvtr_w = 0.0f;

    puts("-- something else on the air (an FT8 slot, PTT): refused before anything moves");
    cfg = (typeof(cfg)){.freq = {14074000, 14074000}, .mode = {radio_mode_usb_dig, radio_mode_usb_dig}, .pwr = 5.0f};
    reset();
    state = RADIO_TX;
    e = run(1500, 1.0f, 15);
    check(e == TUNE_END_REFUSED && seen.keys == 0 && pwr_changes == 0 && as_set(),
          "refused, never keyed, the other's power and all else untouched");
    state = RADIO_RX;
}

/* ---- The GUI's side: the real ext_tune.c, its tune on a thread of its own as on the radio ------------------------ */

static struct {
    float          tune_pwr;    /* Settings > TX Cal > EXTERNAL TUNER */
    int32_t        tune_time;
    int32_t        wspr_hz;
    press_action_t app;         /* the app open, ACTION_NONE none */
    bool           winlink;     /* a Winlink connection under way */
    int32_t        atu;         /* the ATU on (the ATU key, Settings) */
} ui = {.tune_pwr = 1.0f, .tune_time = 15, .wspr_hz = 1500};

ParamFloat *cfg_tune_pwr     = (ParamFloat *)&ui.tune_pwr;
ParamInt   *cfg_tune_time    = (ParamInt *)&ui.tune_time;
ParamInt   *cfg_wspr_tx_freq = (ParamInt *)&ui.wspr_hz;
ParamInt   *cfg_atu_enabled  = (ParamInt *)&ui.atu;
static int  atu_writes;         /* the ATU setting written */
/* The subjects ext_tune.c subscribes to: only their addresses count */
static char subj[7], cur_mode;
ParamInt   *cfg_band_vfoa_freq = (ParamInt *)&subj[0], *cfg_band_vfob_freq = (ParamInt *)&subj[1],
           *cfg_band_vfoa_mode = (ParamInt *)&subj[2], *cfg_band_vfob_mode = (ParamInt *)&subj[3],
           *cfg_band_current_vfo = (ParamInt *)&subj[4], *cfg_band_split = (ParamInt *)&subj[5],
           *cfg_band_tx_shift = (ParamInt *)&subj[6];
ComputedParamInt *cfg_cur_mode = (ComputedParamInt *)&cur_mode;

int32_t param_i_get(const ParamInt *p) {
    return *(const int32_t *)p;
}

/* ext_tune.c sets only the ATU: radio.c's observer sends it to the base at once (on_change_i, HW_ATU_ENABLED) */
void param_i_set(ParamInt *p, int32_t v) {
    *(int32_t *)p = v;
    if (p == cfg_atu_enabled) {
        base.atu = v;
        atu_writes++;
    }
}

int32_t cparam_i_get(const ComputedParamInt *p) {
    (void)p;    /* the mode, the only one ext_tune.c asks for */
    return cfg.mode[cfg.cur];
}

/* Where the radio transmits here: 40 m up to 7.200 (Region 1), 20 m, 10 m */
bool cfg_is_tx_freq(int32_t f) {
    return (f >= 7000000 && f <= 7200000) || (f >= 14000000 && f <= 14350000) || (f >= 28000000 && f <= 29700000);
}

float cfg_pwr_max_get(void) {
    return xvtr_w > 0.0f ? xvtr_w : BOARD_PWR_MAX;
}

static struct {
    Subject    *subj;
    observer_cb fn;
    void       *user_data;
} subs[16];
static int nsubs;

Observer *subject_subscribe(Subject *s, observer_cb fn, void *user_data) {
    if (nsubs < 16) subs[nsubs++] = (typeof(subs[0])){s, fn, user_data};
    return NULL;
}

/* The setting changed: its observers, as the subject calls them on the thread that set it */
static void notify(void *s) {
    for (int i = 0; i < nsubs; i++)
        if (subs[i].subj == s) subs[i].fn(subs[i].subj, subs[i].user_data);
}

button_data_t btn_ext_tune;

void buttons_mark(button_data_t *b, bool on) {
    b->mark = on;
}

void buttons_refresh(button_data_t *b) {
    (void)b;
}

static pthread_mutex_t said_mux = PTHREAD_MUTEX_INITIALIZER;
static char            said[128], told[512];   /* the screen's last message, and all of a tune's */

void msg_update_text_fmt(const char *fmt, ...) {
    va_list ap;

    pthread_mutex_lock(&said_mux);
    va_start(ap, fmt);
    vsnprintf(said, sizeof(said), fmt, ap);
    va_end(ap);
    snprintf(told + strlen(told), sizeof(told) - strlen(told), "%s%s", told[0] ? " | " : "", said);
    printf("        screen: %s\n", said);
    pthread_mutex_unlock(&said_mux);
}

static bool said_is(const char *text) {
    pthread_mutex_lock(&said_mux);
    bool same = !strcmp(said, text);
    pthread_mutex_unlock(&said_mux);
    return same;
}

static atomic_int overs;    /* tunes whose end reached the LVGL thread (over_cb) */

/* The LVGL thread's queue: run at once, on the thread that puts, the argument copied as the queue copies it */
bool scheduler_put_urgent(scheduler_fn_t fn, void *arg, size_t size) {
    unsigned char copy[64];

    if (size > sizeof(copy)) return false;
    memcpy(copy, arg, size);
    fn(copy);
    atomic_fetch_add(&overs, 1);
    return true;
}

press_action_t main_screen_digital_app_action(void) {
    return ui.app;
}

bool winlink_busy(void) {
    return ui.winlink;
}

uint16_t psk_tx_get_center(void) {
    return 1000;
}

uint16_t olivia_tx_get_center(void) {
    return 1500;
}

uint16_t jtty_tx_get_freq(void) {
    return 1200;
}

unsigned radio_swr_trips(void) {
    return (unsigned)seen.trips;
}

/* What happens during the tune (sc.event): on the tune's thread, as the knob's or the Panel's would on theirs */
static bool marked, other_stopped, own_stopped;

static void knob(void) {
    marked = btn_ext_tune.mark && ext_tune_on();
    notify(cfg_band_vfoa_freq);
}

static void mode_key(void) {
    notify(cfg_band_vfoa_mode);
}

static void split_key(void) {
    notify(cfg_band_split);
}

static void shift_set(void) {
    notify(cfg_band_tx_shift);
}

static void key_again(void) {
    ext_tune_toggle();
}

/* The ATU key's press (or the Panel's ATU) during the tune: the ATU on, its observers told */
static void atu_key(void) {
    param_i_set(cfg_atu_enabled, 1);
    notify(cfg_atu_enabled);
}

static void panel_stop(void) {
    other_stopped = ext_tune_stop_owner(TUNE_BY_PANEL + 1, false);     /* another page's Stop */
    own_stopped   = ext_tune_stop_owner(TUNE_BY_PANEL + 2, false);
}

static void panel_gone(void) {
    own_stopped = ext_tune_stop_owner(TUNE_BY_PANEL + 2, true);       /* its lease ran out */
}

/* A tune asked of ext_tune.c by `who` (the key: TUNE_BY_RADIO) on this thread, as the LVGL thread does, and waited
 * for until its end has reached that thread: the refusal, NULL none */
static const char *gui_tune(int who) {
    int         before = atomic_load(&overs);
    const char *no;

    told[0] = 0;
    tune_t0 = now_ms();
    no      = ext_tune_start(who);
    if (no) return no;
    for (int i = 0; i < 5000 && atomic_load(&overs) == before; i++) nanosleep(&(struct timespec){0, 1000000}, NULL);
    printf("        %.2f s on the air, %d key(s), carrier at %d Hz in mode %d\n", (seen.unkey_ms - seen.key_ms) / 1000.0,
           seen.keys, (int)seen.rf, seen.mode);
    return NULL;
}

static double on_air_s(void) {
    return (seen.unkey_ms - seen.key_ms) / 1000.0;
}

static void gui_side(void) {
    const char *no;

    ext_tune_init();
    puts("-- the GUI's side (ext_tune.c): LSB on 7.150, no app, the key; the main knob turned 1 s in");
    cfg = (typeof(cfg)){.freq = {7150000, 7100000}, .mode = {radio_mode_lsb, radio_mode_lsb}, .pwr = 5.0f};
    reset();
    sc.event_at_s = 1.0f;
    sc.event      = knob;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.keys == 1 && seen.rf == 7150000 && seen.max_pwr == 1.0f && strstr(told, "TUNE 1.0 W, 15 s at most"),
          "on the air on 7.150.000 at the TUNE power, 1.0 W, and said so");
    check(marked && !btn_ext_tune.mark && !ext_tune_on(), "the button marked while it tuned, not after");
    check(said_is("TUNE stopped: tuned away") && on_air_s() < 1.2, "stopped at once (%.2f s): \"%s\"", on_air_s(), said);
    check(as_set(), "after: all put back");

    puts("-- the radio's own ATU on, TUNE 1.0 W: the ATU as it is (#87)");
    cfg = (typeof(cfg)){.freq = {14200000, 14200000}, .mode = {radio_mode_usb, radio_mode_usb}, .pwr = 5.0f};
    ui.atu   = 1;
    base.atu = true;
    reset();
    atu_writes    = 0;
    sc.event_at_s = 1.0f;
    sc.event      = key_again;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.keys == 1 && seen.atu && ui.atu && base.atu && atu_writes == 0 &&
          strstr(told, "TUNE 1.0 W, 15 s at most") && as_set(), "keyed with the ATU in, left on: \"%s\"", told);

    puts("-- the ATU on, TUNE 5.0 W: the ATU off before it keys, said so, and off after");
    ui.tune_pwr = 5.0f;
    reset();
    atu_writes    = 0;
    sc.event_at_s = 1.0f;
    sc.event      = key_again;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.keys == 1 && !seen.atu && seen.max_pwr == 5.0f && strstr(told, "TUNE 5.0 W, 15 s, ATU off"),
          "on the air at %.1f W with the ATU out: \"%s\"", seen.max_pwr, told);
    check(!ui.atu && !base.atu && atu_writes == 1 && said_is("TUNE stopped") && as_set(),
          "after: the ATU off still (written once), all else put back");

    puts("-- the ATU off already, TUNE at the radio's PWR top: nothing written");
    ui.tune_pwr = BOARD_PWR_MAX;
    reset();
    atu_writes    = 0;
    sc.event_at_s = 1.0f;
    sc.event      = key_again;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && !seen.atu && atu_writes == 0 && seen.max_pwr == BOARD_PWR_MAX && strstr(told, "15 s at most") &&
          as_set(), "%.1f W, the ATU off as it was: \"%s\"", seen.max_pwr, told);

    puts("-- the ATU on, TUNE 5.0 W refused (something else on the air): the ATU stays on");
    ui.atu   = 1;
    base.atu = true;
    ui.tune_pwr = 5.0f;
    reset();
    atu_writes = 0;
    state      = RADIO_TX;
    no = gui_tune(TUNE_BY_RADIO);
    check(no && ui.atu && base.atu && atu_writes == 0 && said_is("No tune: on the air"), "\"%s\", the ATU on", said);
    state = RADIO_RX;

    puts("-- the ATU on, TUNE 5.0 W on a transverter limited to 0.5 W: a 0.5 W tune, the ATU as it is");
    cfg = (typeof(cfg)){.freq = {28500000, 28500000}, .mode = {radio_mode_usb, radio_mode_usb}, .pwr = 0.5f};
    xvtr_w = 0.5f;
    reset();
    atu_writes    = 0;
    sc.event_at_s = 1.0f;
    sc.event      = key_again;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.atu && ui.atu && atu_writes == 0 && seen.max_pwr == 0.5f && strstr(told, "TUNE 0.5 W, 15 s at most") &&
          as_set(), "%.1f W with the ATU in: \"%s\"", seen.max_pwr, told);
    xvtr_w      = 0.0f;
    ui.atu      = 0;
    base.atu    = false;

    puts("-- TUNE 5.0 W with the ATU off, the ATU key pressed 1 s in: the tune stops, not on through the ATU (#87)");
    cfg = (typeof(cfg)){.freq = {14200000, 14200000}, .mode = {radio_mode_usb, radio_mode_usb}, .pwr = 5.0f};
    ui.tune_pwr = 5.0f;
    reset();
    atu_writes    = 0;
    sc.event_at_s = 1.0f;
    sc.event      = atu_key;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.keys == 1 && !seen.atu && said_is("TUNE stopped: ATU on") && on_air_s() < 1.2,
          "keyed with the ATU out, stopped at once (%.2f s): \"%s\"", on_air_s(), said);
    check(ui.atu && base.atu && as_set(), "after: the ATU on as pressed, all else put back");

    puts("-- TUNE 1.0 W, the ATU key pressed 1 s in: at 1 W the ATU may be in, the tune goes on");
    ui.atu      = 0;
    base.atu    = false;
    ui.tune_pwr = 1.0f;
    reset();
    sc.event_at_s = 1.0f;
    sc.event      = atu_key;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.keys == 1 && ui.atu && said_is("TUNE over: 15 s limit") && on_air_s() > 14.0,
          "on to its limit (%.2f s): \"%s\"", on_air_s(), said);
    ui.atu   = 0;
    base.atu = false;

    puts("-- FT8 open on 14.074 USB-D, its TX at 1500 Hz, the Ext tune button; a mode key 1 s in");
    ui.app                = ACTION_APP_FT8;
    params.ft8_tx_freq.x  = 1500;
    cfg = (typeof(cfg)){.freq = {14074000, 14074000}, .mode = {radio_mode_usb_dig, radio_mode_usb_dig}, .pwr = 5.0f};
    reset();
    sc.event_at_s = 1.0f;
    sc.event      = mode_key;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.rf == 14075500 && said_is("TUNE stopped: mode changed") && as_set(),
          "the tone on 14.075.500, where FT8 transmits; \"%s\"", said);

    puts("-- SSTV open in LSB on 7.171, its picture at 1900 Hz; the key again 1 s in");
    ui.app = ACTION_APP_SSTV;
    cfg = (typeof(cfg)){.freq = {7171000, 7171000}, .mode = {radio_mode_lsb, radio_mode_lsb}, .pwr = 5.0f};
    reset();
    sc.event_at_s = 1.0f;
    sc.event      = key_again;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.rf == 7169100 && said_is("TUNE stopped") && as_set(), "the tone on 7.169.100, below the dial; \"%s\"",
          said);
    ui.app = ACTION_NONE;

    puts("-- split, VFO A 14.200 and B 14.250 USB: split turned off 1 s in");
    cfg = (typeof(cfg)){.freq = {14200000, 14250000}, .mode = {radio_mode_usb, radio_mode_usb}, .split = true,
                        .pwr = 5.0f};
    reset();
    sc.event_at_s = 1.0f;
    sc.event      = split_key;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.rf == 14250000 && said_is("TUNE stopped: VFO changed") && as_set(),
          "the carrier on B's 14.250.000; \"%s\"", said);

    puts("-- a TX shift of +50 kHz on 28.500 USB (split): the shift set to another 1 s in, a TX frequency of its own");
    cfg = (typeof(cfg)){.freq = {28500000, 28500000}, .mode = {radio_mode_usb, radio_mode_usb}, .split = true,
                        .shift = 50000, .pwr = 5.0f};
    reset();
    sc.event_at_s = 1.0f;
    sc.event      = shift_set;
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && seen.rf == 28550000 && said_is("TUNE stopped: tuned away") && on_air_s() < 1.2 && as_set(),
          "on the shift's 28.550.000, stopped at once (%.2f s): \"%s\"", on_air_s(), said);

    puts("-- TUNE time limit 5 s, nothing else");
    cfg = (typeof(cfg)){.freq = {14200000, 14200000}, .mode = {radio_mode_usb, radio_mode_usb}, .pwr = 5.0f};
    ui.tune_time = 5;
    reset();
    no = gui_tune(TUNE_BY_RADIO);
    check(!no && said_is("TUNE over: 5 s limit") && on_air_s() >= 5.0 && on_air_s() < 5.2 && as_set(),
          "over at its limit, %.2f s on the air: \"%s\"", on_air_s(), said);
    ui.tune_time = 99;
    int top = ext_tune_limit_s();
    ui.tune_time = 2;
    check(top == TUNE_TIME_MAX_S && ext_tune_limit_s() == TUNE_TIME_MIN_S, "a time limit out of range told the Panel as "
          "the tune takes it: %d s, %d s", top, ext_tune_limit_s());
    ui.tune_time = 15;

    puts("-- a page's tune (the Panel's client 2): another page's Stop leaves it, its own stops it as asked");
    reset();
    sc.event_at_s = 1.0f;
    sc.event      = panel_stop;
    no = gui_tune(TUNE_BY_PANEL + 2);
    check(!no && !other_stopped && own_stopped && said_is("TUNE stopped") && as_set(), "\"%s\"", said);
    puts("-- the same page gone (its lease ran out, its connection closed)");
    reset();
    sc.event_at_s = 1.0f;
    sc.event      = panel_gone;
    no = gui_tune(TUNE_BY_PANEL + 2);
    check(!no && own_stopped && said_is("TUNE stopped: browser gone") && as_set(), "\"%s\"", said);

    puts("-- refused, the reason on the screen, nothing on the air");
    cfg = (typeof(cfg)){.freq = {7250000, 7250000}, .mode = {radio_mode_lsb, radio_mode_lsb}, .pwr = 5.0f};
    reset();
    state = RADIO_TX;
    no = gui_tune(TUNE_BY_RADIO);
    check(no && !strcmp(no, "no TX on 7.250 MHz") && said_is("No tune: no TX on 7.250 MHz") && !ext_tune_on() &&
          seen.keys == 0, "7.250 where the radio only receives, on the air too: \"%s\"", said);
    cfg = (typeof(cfg)){.freq = {14200000, 14200000}, .mode = {radio_mode_usb, radio_mode_usb}, .pwr = 5.0f};
    reset();
    state = RADIO_TX;
    no = gui_tune(TUNE_BY_RADIO);
    check(no && said_is("No tune: on the air") && !ext_tune_on() && pwr_changes == 0, "something else on the air: \"%s\"",
          said);
    reset();
    ui.winlink = true;
    no = gui_tune(TUNE_BY_RADIO);
    check(no && said_is("No tune: Winlink in session") && !ext_tune_on() && seen.keys == 0, "Winlink in session: \"%s\"",
          said);
    ui.winlink = false;
    cfg = (typeof(cfg)){.freq = {7150000, 7150000}, .mode = {radio_mode_lsb, radio_mode_lsb}, .split = true,
                        .shift = -8000000, .pwr = 5.0f};
    reset();
    no = gui_tune(TUNE_BY_RADIO);
    check(no && said_is("No tune: no TX on -0.850 MHz") && seen.keys == 0, "a TX shift below 0 Hz: \"%s\"", said);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    pure_rules();
    transmissions();
    gui_side();
    printf("%d of %d tune cases as they should be\n", good, cases);
    return good == cases ? 0 : 1;
}
