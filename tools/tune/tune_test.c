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
#include "cfg/cfg_api.h"
#include "params/params.h"
#include "radio.h"
#include "swr_guard.h"
#include "tune_tx.h"
#include "tx_player.h"

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
} sc;

static struct {
    int      keys;          /* times keyed */
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
} seen;

static swr_guard_t swr_guard;
static bool        app_slot_done, app_key_done, app_end_done, tune_key_done, pwr_up_done;
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
    if (digi_cap && w > TX_PLAYER_MAX_PWR_W) w = TX_PLAYER_MAX_PWR_W;
    if (tune_cap > 0.0f && w > tune_cap) w = tune_cap;
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
    app_slot_done = app_key_done = app_end_done = tune_key_done = pwr_up_done = false;
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
    tune_plan(&p, radio_mode_usb, 0, 8.0f, 10.0f, 15);
    check(p.pwr_w == TUNE_PWR_MAX_W, "8 W asked: %.1f W, the setting's top", p.pwr_w);
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

    puts("-- something else on the air (an FT8 slot, PTT): refused before anything moves");
    cfg = (typeof(cfg)){.freq = {14074000, 14074000}, .mode = {radio_mode_usb_dig, radio_mode_usb_dig}, .pwr = 5.0f};
    reset();
    state = RADIO_TX;
    e = run(1500, 1.0f, 15);
    check(e == TUNE_END_REFUSED && seen.keys == 0 && pwr_changes == 0 && as_set(),
          "refused, never keyed, the other's power and all else untouched");
    state = RADIO_RX;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    pure_rules();
    transmissions();
    printf("%d of %d tune cases as they should be\n", good, cases);
    return good == cases ? 0 : 1;
}
