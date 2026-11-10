/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the digital apps' TX level on the X6200 (src/tx_player.c, src/tx_level.c, #126 #123 #16),
 *  and an X6100's with XARA's keys (#120); see the Makefile.
 *
 *  The base: a model X6200 behind the real tx_player.c and tx_level.c. What it is driven with is the audio played, the
 *  samples' gain and the codec's (the X6200's AIF1 DA0: -120 to +24 dB in 0.75 dB steps, its output cut at full
 *  scale), as a level in dB from the default's (the level 0 dB of the apps, where the stock X6200 gives its setting at
 *  once). From its knee up the base holds the power at the setting itself; under it the power falls with the drive.
 *  After the key it comes up at most so fast (PR #140: from 0 in 0.1 W steps after each move); the band, the antenna
 *  or the supply may give less than the setting; its readings come 20 a second (2.5 in some scenarios), the power in
 *  whole tenths cut off, with noise; the ALC as the scenario reads it (the X6200's reads about 0 whatever the drive),
 *  or an X6100's, which follows the drive from the knee up; the readings at once or a little late. Each scenario sends
 *  FT8 transmissions of 12.64 s in a row, or XARA's keys that wait out the other station's CW ID in silence first, the
 *  learned level carried from one to the next as on the radio. The clock is the test's own, moved on by the audio
 *  played.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "cfg/cfg_api.h"
#include "msg.h"
#include "params/params.h"
#include "radio.h"
#include "tx_level.h"
#include "tx_meters.h"
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

/* ---- The settings ------------------------------------------------------------------------------------------- */

static float setting;       /* PWR */
ParamFloat  *cfg_pwr = (ParamFloat *)&setting;
params_t     params  = {.line_in = 10};

float param_f_get(const ParamFloat *p) {
    return *(const float *)p;
}

float cfg_pwr_max_get(void) {
    return 8.0f;    /* the X6200 on an external supply */
}

void params_float_set(params_float_t *p, float v) {
    p->x = v;
}

static int zero_w_said;     /* "TX 0 W" on the screen */

void msg_schedule_text_fmt(const char *fmt, ...) {
    zero_w_said += !strncmp(fmt, "TX 0 W", 6);
}

/* ---- The model X6200 ---------------------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    float       pwr;        /* PWR, W */
    float       knee_db;    /* the drive (dB from the default's) from which the base holds its setting */
    float       reach_w;    /* the most this band, antenna and supply give, 0 the setting */
    float       ramp_w_s;   /* after the key the power comes up at most this fast, W a second, 0 at once */
    float       alc;        /* the ALC read while on the air... */
    float       alc_share;  /* ...in this share of the readings (RF picked up now and then), 0 never */
    float       noise_w;    /* the power reading's noise, +- W, before its cut to tenths */
    float       over;       /* the power read is the output times this, 0 as 1 */
    float       start_db;   /* the learned level the first transmission starts from */
    int         txs;        /* transmissions in a row */
    int         read_every; /* a reading every this many 50 ms, 0 as 1 (20 a second): fewer, and their noise shows */
    float       quiet_s;    /* each key this long in silence, then 2.6 s of audio (XARA after the other's QRT), 0 FT8 */
    float       alc_db;     /* an X6100: its ALC follows the drive, this much a dB over the knee; 0 the X6200's */
    float       lag_s;      /* the readings show the base this long before (the audio queued, the base's own lag) */
} scenario_t;

static const scenario_t *sc;

static struct {
    bool     modem;
    float    pwr;           /* what tx_player set, 0 the setting */
    uint64_t key_ns;
    float    codec_db;      /* the codec's playback level */
    float    drive_db;      /* the last audio's level, dB from the default's */
    bool     clipped;       /* the codec's output went over full scale */
} base;

/* One transmission as the meters saw it */
static struct {
    float at2;              /* the power read 2 s in */
    float min, max;         /* after 1.5 s */
    float sum_last;         /* the last 3 s */
    int   n_last;
    bool  clipped;
    float top_drive;        /* the most the base was driven with, dB from the default's */
} tx;

static uint8_t  reading_id;
static float    reading_pwr, reading_alc;
static unsigned rnd = 1;
static float    stop_at_s;      /* radio.c's band_stop this far into a transmission (#121), 0 none */
static unsigned band_stops;

static float rnd01(void) {
    rnd = rnd * 1103515245u + 12345u;
    return ((rnd >> 8) & 0xFFFF) / 65536.0f;
}

/* The mode (#224): the one in use, what radio_set_tx_mode set for the transmission (-1 none), the base's as it keyed,
 * and the calls: none while keyed */
static int32_t    mode_in_use = radio_mode_usb_dig;
ComputedParamInt *cfg_cur_mode = (ComputedParamInt *)&mode_in_use;
static int        tx_mode_set = -1, mode_at_key = -1, mode_calls;
static bool       mode_on_air;

int32_t cparam_i_get(const ComputedParamInt *p) {
    return *(const int32_t *)p;
}

void radio_set_tx_mode(int mode) {
    tx_mode_set = mode;
    mode_calls++;
    mode_on_air |= base.modem;
}

int radio_tx_mode(void) {
    return tx_mode_set;
}

radio_state_t radio_get_state(void) {
    return base.modem ? RADIO_TX : RADIO_RX;
}

bool radio_get_modem(void) {
    return base.modem;
}

void radio_set_modem(bool on) {
    if (on && !base.modem) {
        base.key_ns = atomic_load(&virt_ns);
        mode_at_key = tx_mode_set >= 0 ? tx_mode_set : mode_in_use;
    }
    base.modem = on;
}

int32_t radio_tx_freq(void) {
    return 3573000;
}

void radio_set_tx_freq(int32_t f) {
    (void)f;
}

void radio_set_pwr(float w) {
    base.pwr = w;
}

uint32_t radio_caps(void) {
    return sc->alc_db > 0.0f ? HW_CAP_TX_LEVEL : HW_CAP_TX_LEVEL | HW_CAP_TX_NO_ALC;
}

unsigned radio_band_stops(void) {
    return band_stops;
}

/* The X6200's AIF1 DA0 */
float audio_set_play_vol(float db) {
    float d = db > 24.0f ? 24.0f : db < -120.0f ? -120.0f : db;

    base.codec_db = roundf(d / 0.75f) * 0.75f;
    return base.codec_db;
}

void audio_gain_db(int16_t *buf, size_t n, float g, int16_t *out) {
    float k = powf(10.0f, g / 20.0f);

    for (size_t i = 0; i < n; i++) out[i] = (int16_t)lrintf(fmaxf(-32767.0f, fminf(32767.0f, buf[i] * k)));
}

void audio_gain_db_transition(int16_t *buf, size_t n, float g1, float g2, int16_t *out) {
    for (size_t i = 0; i < n; i++) {
        float k = powf(10.0f, (g1 + (g2 - g1) * i / n) / 20.0f);

        out[i] = (int16_t)lrintf(fmaxf(-32767.0f, fminf(32767.0f, buf[i] * k)));
    }
}

/* The FT8 tone at 0.8 of full scale (gfsk.c) at the default level: what the stock X6200 is driven with */
#define TONE_AMP    (32767.0f * 0.8f)
#define DEFAULT_RMS (TONE_AMP / sqrtf(2.0f) * powf(10.0f, -9.4f / 20.0f))

/* The drive of the last 3.2 s, a 50 ms tick each, for readings that come late */
#define DRIVE_HIST 64

static float    drive_hist[DRIVE_HIST];
static unsigned drive_ticks;

/* One reading of the base: what it puts out for the drive, after the key, at the band's top */
static void base_reading(void) {
    float s     = (atomic_load(&virt_ns) - base.key_ns) / 1e9f;
    float set   = base.pwr > 0.0f ? base.pwr : setting;
    float drive = drive_hist[(drive_ticks - 1 - (unsigned)lrintf(sc->lag_s / 0.05f)) % DRIVE_HIST];
    float p     = drive >= sc->knee_db ? set : set * powf(10.0f, (drive - sc->knee_db) / 10.0f);

    if (sc->reach_w > 0.0f && p > sc->reach_w) p = sc->reach_w;
    if (sc->ramp_w_s > 0.0f) p = fminf(p, floorf(s * sc->ramp_w_s * 10.0f) / 10.0f);
    p *= sc->over > 0.0f ? sc->over : 1.0f;
    p += sc->noise_w * (2.0f * rnd01() - 1.0f);
    reading_pwr = p > 0.0f ? floorf(p * 10.0f + 1e-4f) / 10.0f : 0.0f;
    reading_alc = sc->alc_share > 0.0f && rnd01() < sc->alc_share ? sc->alc : 0.0f;
    if (sc->alc_db > 0.0f) reading_alc = fminf(10.0f, fmaxf(reading_alc, (drive - sc->knee_db) * sc->alc_db));
    reading_id++;

    if (s >= 2.0f && tx.at2 < 0.0f) tx.at2 = reading_pwr;
    if (s >= 1.5f) {
        tx.min = fminf(tx.min, reading_pwr);
        tx.max = fmaxf(tx.max, reading_pwr);
    }
    if (s >= 12.64f - 3.0f) {
        tx.sum_last += reading_pwr;
        tx.n_last++;
    }
}

bool tx_info_refresh(uint8_t *s, float *alc, float *pwr, float *swr) {
    if (*s == reading_id) return false;
    *s = reading_id;
    if (alc) *alc = reading_alc;
    if (pwr) *pwr = reading_pwr;
    if (swr) *swr = 1.1f;
    return true;
}

void tx_info_set_length(float s) {
    (void)s;
}

/* The audio goes out in real time: the clock moves on by it, the base reads 20 times a second */
int audio_play(int16_t *buf, size_t n) {
    static uint64_t carry_ns;
    float           k = powf(10.0f, base.codec_db / 20.0f);
    double          sum = 0.0;

    for (size_t i = 0; i < n; i++) {
        float v = buf[i] * k;

        if (fabsf(v) > 32767.0f) {
            v = copysignf(32767.0f, v);
            base.clipped = tx.clipped = true;
        }
        sum += (double)v * v;
    }
    base.drive_db = 10.0f * log10f((float)(sum / n) / (DEFAULT_RMS * DEFAULT_RMS) + 1e-12f);
    tx.top_drive  = fmaxf(tx.top_drive, base.drive_db);

    static unsigned ticks;

    carry_ns += (uint64_t)n * 1000000000ULL / AUDIO_PLAY_RATE;
    while (carry_ns >= 50000000ULL) {
        carry_ns -= 50000000ULL;
        atomic_fetch_add(&virt_ns, 50000000ULL);
        drive_hist[drive_ticks++ % DRIVE_HIST] = base.drive_db;
        if (base.modem && ++ticks % (unsigned)(sc->read_every > 0 ? sc->read_every : 1) == 0) base_reading();
        if (base.modem && stop_at_s > 0.0f && (atomic_load(&virt_ns) - base.key_ns) / 1e9f >= stop_at_s) {
            band_stops++;       /* as band_stop: the count, then the modem bit */
            base.modem = false;
        }
    }
    return (int)n;
}

void audio_play_wait(void) {
}

/* ---- The transmissions -------------------------------------------------------------------------------------- */

#define FT8_SAMPLES (AUDIO_PLAY_RATE * 1264 / 100)    /* 12.64 s */

static int16_t ft8[FT8_SAMPLES];

/* XARA's key after the other station's QRT (code 3): keyed in silence while that station identifies (9 to 12 blank
 * words, 2.7-3.6 s), then its own CW ID, here 2.6 s of tone */
#define ID_SAMPLES (AUDIO_PLAY_RATE * 26 / 10)

static int16_t idkey[AUDIO_PLAY_RATE * 4 + ID_SAMPLES];

/* One FT8 transmission, or one such key; the base starts from 0 W, as after each move */
static void send(void) {
    tx = (typeof(tx)){.at2 = -1.0f, .min = 99.0f, .top_drive = -99.0f};
    base.drive_db = -99.0f;
    for (int i = 0; i < DRIVE_HIST; i++) drive_hist[i] = -99.0f;
    if (sc->quiet_s > 0.0f) {
        uint32_t quiet = (uint32_t)(sc->quiet_s * AUDIO_PLAY_RATE);

        memset(idkey, 0, quiet * sizeof(int16_t));
        memcpy(idkey + quiet, ft8, ID_SAMPLES * sizeof(int16_t));
        tx_player_play(idkey, quiet + ID_SAMPLES, 1325, tx_player_base_gain_offset(), NULL, NULL);
    } else {
        tx_player_play(ft8, FT8_SAMPLES, 1325, tx_player_base_gain_offset(), NULL, NULL);
    }
    atomic_fetch_add(&virt_ns, 2360000000ULL);      /* the rest of the slot */
}

typedef struct {
    float first_at2, last_at2;      /* the power 2 s in, first and last transmission */
    float lowest;                   /* the lowest power read after 1.5 s, all transmissions */
    float level_top, level_end;     /* the learned level: the highest, and after the last */
    float drive_top;                /* the most the base was driven with, dB from the default's */
    int   clipped;                  /* transmissions whose audio went over the codec's full scale */
    int   zero_w;                   /* "TX 0 W" said */
    int   short_txs;                /* transmissions whose last 3 s averaged under 90 % of the setting */
} result_t;

static result_t run(const scenario_t *s, bool verbose) {
    result_t r = {.lowest = 99.0f, .level_top = -99.0f, .drive_top = -99.0f};

    sc          = s;
    setting     = s->pwr;
    base        = (typeof(base)){0};
    zero_w_said = 0;
    rnd         = 1;
    params.ft8_output_gain_offset.x = s->start_db;
    if (verbose) printf("  %s\n", s->name);
    for (int i = 0; i < s->txs; i++) {
        float before = params.ft8_output_gain_offset.x;

        send();

        float last = tx.n_last ? tx.sum_last / tx.n_last : 0.0f;

        if (i == 0) r.first_at2 = tx.at2;
        r.last_at2  = tx.at2;
        r.lowest    = fminf(r.lowest, tx.min);
        r.level_top = fmaxf(r.level_top, params.ft8_output_gain_offset.x);
        r.drive_top = fmaxf(r.drive_top, tx.top_drive);
        r.clipped  += tx.clipped;
        r.short_txs += last < 0.9f * fminf(s->pwr, s->reach_w > 0.0f ? s->reach_w : s->pwr);
        if (verbose && (i < 4 || i == s->txs - 1 || i % 10 == 9))
            printf("      tx %2d: level %6.1f -> %6.1f dB, power 2 s in %.1f W, %.1f-%.1f W, last 3 s %.2f W%s\n",
                   i + 1, before, params.ft8_output_gain_offset.x, tx.at2, tx.min, tx.max, last,
                   tx.clipped ? ", clipped" : "");
    }
    r.level_end = params.ft8_output_gain_offset.x;
    r.zero_w    = zero_w_said;
    return r;
}

/* The same, and what tx_player logged meanwhile (its stderr) in log */
static result_t run_logged(const scenario_t *s, bool verbose, char *log, size_t size) {
    FILE    *t     = tmpfile();
    int      saved = dup(2);
    result_t r;

    fflush(stderr);
    dup2(fileno(t), 2);
    r = run(s, verbose);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    rewind(t);
    log[fread(log, 1, size - 1, t)] = 0;
    fclose(t);
    return r;
}

/* tx_player's line for each key that moved the level: how many, and whether each decided only in the audio after its
 * silence (blocks of 2048 samples, the first TX_LEVEL_QUIET_BLOCKS of sound after it not counted), every window at
 * most a fast step up */
static int level_lines(const char *log, int silent_want, int *bad) {
    int n = 0;

    *bad = 0;
    for (const char *l = log; (l = strstr(l, "tx_player: level ")); l++) {
        float from, to;
        int   windows, blocks, silent;

        if (sscanf(l, "tx_player: level %f -> %f dB, %d windows decided in %d blocks, %d of them silent", &from, &to,
                   &windows, &blocks, &silent) != 5) {
            (*bad)++;
            continue;
        }
        n++;
        *bad += silent != silent_want || windows > (blocks - silent - TX_LEVEL_QUIET_BLOCKS) / TX_LEVEL_WINDOW_BLOCKS
             || to - from > windows * TX_LEVEL_UP_FAST_DB + 0.01f;
    }
    return n;
}

int main(int argc, char **argv) {
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");

    for (uint32_t i = 0; i < FT8_SAMPLES; i++)
        ft8[i] = (int16_t)lrintf(TONE_AMP * sinf(2.0f * (float)M_PI * TX_PLAYER_AUDIO_HZ * i / AUDIO_PLAY_RATE));

    static const scenario_t bench = {"the bench X6200 into a dummy load, 7 W, up in about 1 s", 7.0f, -6.0f, 0, 7.0f,
                                     0, 0, 0, 0, 0.0f, 30};
    static const scenario_t ceiling = {"#126's radio: the learned level at the codec's top, 33.4 dB", 7.0f, -6.0f, 0,
                                       7.0f, 0, 0, 0, 0, 33.4f, 30};
    static const scenario_t ramp = {"the power up in 3.5 s at 7 W (0.1 W steps, 2 W a second)", 7.0f, -6.0f, 0, 2.0f,
                                    0, 0, 0, 0, 0.0f, 40};
    static const scenario_t reach = {"7 W asked, 6 W the most on this band, readings +-0.15 W", 7.0f, -6.0f, 6.0f, 7.0f,
                                     0, 0, 0.15f, 0, 0.0f, 60};
    static const scenario_t pickup = {"RF picked up on 80 m: ALC 10 in 40 % of the readings, 7 W", 7.0f, -6.0f, 0,
                                      7.0f, 10.0f, 0.4f, 0, 0, 0.0f, 30};
    static const scenario_t pickup2 = {"the same at 2 W, ALC 6 in two thirds of the readings", 2.0f, -6.0f, 0, 2.0f,
                                       6.0f, 0.67f, 0, 0, 0.0f, 30};
    static const scenario_t needy = {"a radio that needs 4 dB over the default (#16), 5 W", 5.0f, 4.0f, 0, 5.0f, 0, 0,
                                     0.05f, 0, 0.0f, 20};
    static const scenario_t needy_slow = {"the same, its power up in 2.5 s (2 W a second)", 5.0f, 4.0f, 0, 2.0f, 0, 0,
                                          0.05f, 0, 0.0f, 20};
    static const scenario_t knocked = {"a level knocked 16 dB down (#16), 5 W", 5.0f, -6.0f, 0, 5.0f, 0, 0, 0, 0,
                                       -16.0f, 10};
    static const scenario_t overread = {"the power read 30 % over, 5 W", 5.0f, -6.0f, 0, 5.0f, 0, 0, 0, 1.3f, 0.0f, 30};
    static const scenario_t reach_slow = {"7 W asked, 6 W on this band, readings +-0.15 W 2.5 a second", 7.0f, -6.0f,
                                          6.0f, 7.0f, 0, 0, 0.15f, 0, 0.0f, 100, 8};
    static const scenario_t hover = {"the power a hair under 7 W (6.95), readings +-0.1 W 2.5 a second", 7.0f, -6.0f,
                                     6.95f, 7.0f, 0, 0, 0.1f, 0, 0.0f, 300, 8};
    static const scenario_t needy_noisy = {"a radio that needs 4 dB over the default, readings +-0.1 W 2.5 a second",
                                           5.0f, 4.0f, 0, 7.0f, 0, 0, 0.1f, 0, 0.0f, 40, 8};
    static const scenario_t needy_reach = {"the same, 4.5 W the most at 5 W asked", 5.0f, 4.0f, 4.5f, 7.0f, 0, 0,
                                           0.1f, 0, 0.0f, 40, 8};

    result_t r;

    printf("The level across transmissions: FT8 on the X6200, then an X6100, and XARA's keys with silence\n");
    r = run(&bench, verbose);
    check(r.first_at2 >= 6.9f && r.lowest >= 6.9f, "%s: %.1f W from the first, never under %.1f W", bench.name,
          r.first_at2, r.lowest);
    check(fabsf(r.level_end) <= 0.5f && r.clipped == 0, "%s: the level stays at %.1f dB, the audio unclipped",
          bench.name, r.level_end);

    r = run(&ceiling, verbose);
    check(r.clipped == 0, "%s: no transmission clipped in the codec (%d did)", ceiling.name, r.clipped);
    check(r.level_end <= 9.5f && r.drive_top <= 9.5f, "%s: the level %.1f dB, the drive at most %.1f dB over the default",
          ceiling.name, r.level_end, r.drive_top);
    check(r.lowest >= 6.9f, "%s: still 7 W, never under %.1f W", ceiling.name, r.lowest);

    r = run(&ramp, verbose);
    check(r.level_top <= 9.5f && r.clipped == 0, "%s: the level at most %.1f dB, %d clipped", ramp.name, r.level_top,
          r.clipped);
    check(r.short_txs == 0, "%s: every transmission ends at the setting (%d short)", ramp.name, r.short_txs);

    r = run(&reach, verbose);
    check(r.level_top <= 9.5f && r.clipped == 0, "%s: the level at most %.1f dB, %d clipped", reach.name, r.level_top,
          r.clipped);
    check(r.short_txs == 0, "%s: every transmission ends at the band's 6 W (%d short)", reach.name, r.short_txs);

    r = run(&pickup, verbose);
    check(r.zero_w == 0 && r.lowest >= 6.9f, "%s: never under %.1f W, \"TX 0 W\" %d times", pickup.name, r.lowest,
          r.zero_w);
    check(r.short_txs == 0, "%s: no transmission short (%d)", pickup.name, r.short_txs);

    r = run(&pickup2, verbose);
    check(r.zero_w == 0 && r.lowest >= 1.9f, "%s: never under %.1f W, \"TX 0 W\" %d times", pickup2.name, r.lowest,
          r.zero_w);

    r = run(&needy, verbose);
    check(r.last_at2 >= 4.9f, "%s: %.1f W 2 s in from the second transmission on", needy.name, r.last_at2);
    check(r.short_txs <= 1, "%s: at most the first short (%d)", needy.name, r.short_txs);

    r = run(&needy_slow, verbose);
    check(r.level_end >= 3.5f && r.level_end <= 5.0f && r.short_txs <= 1,
          "%s: the level %.1f dB, where the power comes, %d short", needy_slow.name, r.level_end, r.short_txs);

    r = run(&knocked, verbose);
    check(r.last_at2 >= 4.9f && r.short_txs <= 1, "%s: back at %.1f W, %d short", knocked.name, r.last_at2,
          r.short_txs);

    /* The radio stopped the transmission 6 s in, moved outside the amateur bands (#121): what it climbed is not kept */
    static const scenario_t knocked1 = {"the same, one transmission", 5.0f, -6.0f, 0, 5.0f, 0, 0, 0, 0, -16.0f, 1, 0};
    r = run(&knocked1, verbose);
    float climbed = r.level_end;
    stop_at_s = 6.0f;
    r = run(&knocked1, verbose);
    stop_at_s = 0.0f;
    check(climbed > -16.0f && r.level_end == -16.0f, "%s, stopped by the radio 6 s in: the level stays at %.1f dB "
          "(%.1f dB without the stop)", knocked1.name, r.level_end, climbed);

    r = run(&overread, verbose);
    check(r.zero_w == 0 && r.short_txs <= 2, "%s: the level comes down to where the reading fits, %d short, "
          "\"TX 0 W\" %d times", overread.name, r.short_txs, r.zero_w);

    /* The readings' noise and 0.1 W steps against the steps up: none kept that brought nothing, a needed one kept */
    r = run(&reach_slow, verbose);
    check(r.level_top <= 0.5f && r.short_txs == 0, "%s: the level at most %.1f dB, %d short", reach_slow.name,
          r.level_top, r.short_txs);

    r = run(&hover, verbose);
    check(r.level_end <= 0.5f && r.short_txs == 0, "%s: the level %.1f dB after 300, %d short", hover.name, r.level_end,
          r.short_txs);

    r = run(&needy_noisy, verbose);
    check(r.level_end >= 3.5f && r.level_end <= 4.5f && r.short_txs <= 1, "%s: the level %.1f dB, %d short",
          needy_noisy.name, r.level_end, r.short_txs);

    r = run(&needy_reach, verbose);
    check(r.level_end >= 3.0f && r.level_end <= 4.5f && r.short_txs <= 1, "%s: the level %.1f dB, %d short",
          needy_reach.name, r.level_end, r.short_txs);

    /* An X6100's ALC follows the drive: the level settles where it reads about 1 */
    static const scenario_t x6100 = {.name = "an X6100 (its ALC 0.6 a dB over the knee), 5 W", .pwr = 5.0f,
                                     .knee_db = -6.0f, .start_db = -8.0f, .txs = 10, .alc_db = 0.6f};
    r = run(&x6100, verbose);
    check(r.level_end >= -4.75f && r.level_end <= -3.5f && r.short_txs <= 1, "%s: the level %.1f dB, ALC about 1, "
          "%d short", x6100.name, r.level_end, r.short_txs);

    /* XARA's keys after the other station's QRT: the silence before its CW ID is not a power short of the setting.
     * It moved the level up 0.5 dB a window, kept for every app: on the X6100 towards where the ALC reads 3, on the
     * X6200 to the codec's top. The readings come half a second late */
    static const scenario_t id_x6100 = {.name = "the same, XARA's keys with 3 s of silence before the CW ID",
                                        .pwr = 5.0f, .knee_db = -6.0f, .start_db = -4.0f, .txs = 30, .quiet_s = 3.0f,
                                        .alc_db = 0.6f, .lag_s = 0.5f};
    r = run(&id_x6100, verbose);
    check(r.level_top <= -3.5f && r.level_end >= -4.5f, "%s: the level %.1f dB after %d, at most %.1f dB",
          id_x6100.name, r.level_end, id_x6100.txs, r.level_top);

    /* Their audio still counts: from 2.5 dB under, they bring the level up to where the ALC reads about 1 */
    static const scenario_t id_low = {.name = "the same, from 2.5 dB under", .pwr = 5.0f, .knee_db = -6.0f,
                                      .start_db = -6.5f, .txs = 30, .quiet_s = 3.0f, .alc_db = 0.6f, .lag_s = 0.5f};
    static char             log[16384];
    int                     bad, silent = (int)(id_low.quiet_s * AUDIO_PLAY_RATE) / 2048;

    r = run_logged(&id_low, verbose, log, sizeof(log));
    check(r.level_end >= -4.75f && r.level_end <= -3.5f, "%s: the level %.1f dB after %d", id_low.name, r.level_end,
          id_low.txs);
    /* What the bench reads in the GUI's log: before, about 7 windows of such a key were decided, 4 in its silence */
    int lines = level_lines(log, silent, &bad);
    check(lines > 0 && bad == 0, "%s: %d keys logged the level they moved, each with its %d blocks of silence and "
          "windows decided only in the audio after them (%d not)", id_low.name, lines, silent, bad);

    static const scenario_t id_x6200 = {.name = "the bench X6200, XARA's keys with 3.6 s of silence before the CW ID",
                                        .pwr = 7.0f, .knee_db = -6.0f, .ramp_w_s = 7.0f, .txs = 30, .quiet_s = 3.6f,
                                        .lag_s = 0.5f};
    r = run(&id_x6200, verbose);
    check(r.level_top <= 0.5f && r.level_end >= -0.5f, "%s: the level %.1f dB after %d, at most %.1f dB",
          id_x6200.name, r.level_end, id_x6200.txs, r.level_top);

    /* The mode (#224): an app in LSB or USB (SSTV, RTTY, WeFax keep the radio's) goes out in LSB-D or USB-D, without
     * the base's compressor, and the mode is back after the unkey; a data mode, CW and FM as they are, and a TUNE's
     * own USB-D (tune_tx.c sets it before and puts it back after) left to it */
    static const scenario_t one = {.name = "one transmission", .pwr = 5.0f, .knee_db = -6.0f, .txs = 1,
                                   .alc_db = 0.6f};
    static const struct {
        const char *name;
        int         in_use, set_before, keyed;
    } modes[] = {
        {"LSB (SSTV on 40 m, RTTY)", radio_mode_lsb, -1, radio_mode_lsb_dig},
        {"USB (SSTV on 20 m, WeFax, RTTY in USB)", radio_mode_usb, -1, radio_mode_usb_dig},
        {"USB-D (FT8)", radio_mode_usb_dig, -1, radio_mode_usb_dig},
        {"LSB-D", radio_mode_lsb_dig, -1, radio_mode_lsb_dig},
        {"CW", radio_mode_cw, -1, radio_mode_cw},
        {"FM", radio_mode_nfm, -1, radio_mode_nfm},
        {"a TUNE in LSB, in its own USB-D", radio_mode_lsb, radio_mode_usb_dig, radio_mode_usb_dig},
    };

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        bool changed = modes[i].set_before < 0 && modes[i].keyed != modes[i].in_use;

        mode_in_use = modes[i].in_use;
        tx_mode_set = modes[i].set_before;
        mode_at_key = -1;
        mode_calls  = 0;
        mode_on_air = false;
        run(&one, false);
        check(mode_at_key == modes[i].keyed && !mode_on_air && tx_mode_set == modes[i].set_before
              && mode_calls == (changed ? 2 : 0), "%s: keyed in mode %d (%d wanted), %d calls, none on the air: %s, "
              "after it %d", modes[i].name, mode_at_key, modes[i].keyed, mode_calls, mode_on_air ? "no" : "yes",
              tx_mode_set);
    }
    mode_in_use = radio_mode_usb_dig;
    tx_mode_set = -1;

    printf("TXLEVEL %s: %d of %d cases\n", good == cases ? "OK" : "FAILED", good, cases);
    return good == cases ? 0 : 1;
}
