/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the digital apps' TX level on the X6200 (src/tx_player.c, src/tx_level.c, #126 #123 #16);
 *  see the Makefile.
 *
 *  The base: a model X6200 behind the real tx_player.c and tx_level.c. What it is driven with is the audio played, the
 *  samples' gain and the codec's (the X6200's AIF1 DA0: -120 to +24 dB in 0.75 dB steps, its output cut at full
 *  scale), as a level in dB from the default's (the level 0 dB of the apps, where the stock X6200 gives its setting at
 *  once). From its knee up the base holds the power at the setting itself; under it the power falls with the drive.
 *  After the key it comes up at most so fast (PR #140: from 0 in 0.1 W steps after each move); the band, the antenna
 *  or the supply may give less than the setting; its readings come 20 a second, the power in whole tenths cut off, with
 *  noise; the ALC as the scenario reads it (the X6200's reads about 0 whatever the drive). Each scenario sends FT8
 *  transmissions of 12.64 s in a row, the learned level carried from one to the next as on the radio. The clock is
 *  the test's own, moved on by the audio played.
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

#include "audio.h"
#include "cfg/cfg_api.h"
#include "msg.h"
#include "params/params.h"
#include "radio.h"
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

static float rnd01(void) {
    rnd = rnd * 1103515245u + 12345u;
    return ((rnd >> 8) & 0xFFFF) / 65536.0f;
}

radio_state_t radio_get_state(void) {
    return base.modem ? RADIO_TX : RADIO_RX;
}

bool radio_get_modem(void) {
    return base.modem;
}

void radio_set_modem(bool on) {
    if (on && !base.modem) base.key_ns = atomic_load(&virt_ns);
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
    return HW_CAP_TX_LEVEL | HW_CAP_TX_NO_ALC;
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

/* One reading of the base: what it puts out for the drive, after the key, at the band's top */
static void base_reading(void) {
    float s   = (atomic_load(&virt_ns) - base.key_ns) / 1e9f;
    float set = base.pwr > 0.0f ? base.pwr : setting;
    float p   = base.drive_db >= sc->knee_db ? set : set * powf(10.0f, (base.drive_db - sc->knee_db) / 10.0f);

    if (sc->reach_w > 0.0f && p > sc->reach_w) p = sc->reach_w;
    if (sc->ramp_w_s > 0.0f) p = fminf(p, floorf(s * sc->ramp_w_s * 10.0f) / 10.0f);
    p *= sc->over > 0.0f ? sc->over : 1.0f;
    p += sc->noise_w * (2.0f * rnd01() - 1.0f);
    reading_pwr = p > 0.0f ? floorf(p * 10.0f + 1e-4f) / 10.0f : 0.0f;
    reading_alc = sc->alc_share > 0.0f && rnd01() < sc->alc_share ? sc->alc : 0.0f;
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

    carry_ns += (uint64_t)n * 1000000000ULL / AUDIO_PLAY_RATE;
    while (carry_ns >= 50000000ULL) {
        carry_ns -= 50000000ULL;
        atomic_fetch_add(&virt_ns, 50000000ULL);
        if (base.modem) base_reading();
    }
    return (int)n;
}

void audio_play_wait(void) {
}

/* ---- The transmissions -------------------------------------------------------------------------------------- */

#define FT8_SAMPLES (AUDIO_PLAY_RATE * 1264 / 100)    /* 12.64 s */

static int16_t ft8[FT8_SAMPLES];

/* One FT8 transmission; the base starts from 0 W, as after each move */
static void send(void) {
    tx = (typeof(tx)){.at2 = -1.0f, .min = 99.0f, .top_drive = -99.0f};
    base.drive_db = -99.0f;
    tx_player_play(ft8, FT8_SAMPLES, 1325, tx_player_base_gain_offset(), NULL, NULL);
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

    result_t r;

    printf("The X6200's level across FT8 transmissions\n");
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

    r = run(&overread, verbose);
    check(r.zero_w == 0 && r.short_txs <= 2, "%s: the level comes down to where the reading fits, %d short, "
          "\"TX 0 W\" %d times", overread.name, r.short_txs, r.zero_w);

    printf("TXLEVEL %s: %d of %d cases\n", good == cases ? "OK" : "FAILED", good, cases);
    return good == cases ? 0 : 1;
}
