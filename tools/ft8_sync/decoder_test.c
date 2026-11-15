/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the times Auto Sync goes by (src/ft8/time_sync.h), with the real decoder (src/ft8/worker.c and
 *  ft8_lib) and the real decoder thread (src/ft8/audio_worker.c).
 *
 *  1. The decoder's lag: an FT8 and an FT4 message made by the app's own encoder, put at 64 places from 0.3 to 1.1 s
 *     into the decoder's audio, over noise. The time it gives is late by FTX_SYNC_DECODER_LAG (1.2 symbols) on
 *     average (within 0.1 symbol), each within 0.6 symbol of it (its time step is a quarter symbol).
 *  2. The decoder thread, in real time: FT4 audio in fragments of 0.1 s as PulseAudio gives them, a signal 0.3 s late
 *     in every slot by the clock, and every other slot the thread kept 0.3 to 0.5 s past the slot's end (a long last
 *     decode, a busy radio), so that its audio of the next slot starts that much later. The time the decoder gives
 *     moves with that start; the DT from the audio's stamp (slot_info_t's rx_start_ns) stays 0.3 s. Takes about 30 s.
 *
 *    decoder_test
 */

#include "ft8/audio_worker.h"
#include "ft8/ft2.h"
#include "ft8/time_sync.h"
#include "ft8/utils.h"
#include "ft8/worker.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RATE    11025               /* the app's: 44.1 kHz / 4 */
#define NS      1000000000LL
#define FT8     ((int)FTX_PROTOCOL_FT8)
#define FT4     ((int)FTX_PROTOCOL_FT4)

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

/* audio_worker.c's milliseconds (util.h) */
uint64_t get_time(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static float noise(void) {
    return 0.01f * ((float)rand() / (float)RAND_MAX - 0.5f);
}

/* ---- 1. the decoder's lag ---- */

static float found_t;
static int   found;

static void lag_cb(const char *text, int snr, float freq_hz, float time_sec, void *user) {
    (void)text, (void)snr, (void)freq_hz, (void)user;
    if (!found++) found_t = time_sec;
}

static void decoder_lag(int proto) {
    float    symbol = proto == FT8 ? FT8_SYMBOL_PERIOD : FT4_SYMBOL_PERIOD, sum = 0.0f, worst = 0.0f;
    int16_t *tx;
    uint32_t ntx;
    int      decoded = 0;
    char     what[96];

    ftx_worker_init(RATE, proto);
    if (!ftx_worker_generate_tx_samples("CQ ON6URE JO21", 1500, RATE, &tx, &ntx)) {
        check(false, "the encoder makes the message");
        ftx_worker_free();
        return;
    }

    int    n   = (int)(ftx_slot_time(proto) * RATE);
    float *buf = malloc(n * sizeof(float));
    int    bs  = ftx_worker_get_block_size();

    for (int i = 0; i < 64; i++) {
        float start = 0.3f + i * 0.0123f;
        int   s0    = (int)lroundf(start * RATE);

        srand(i + 1);
        for (int k = 0; k < n; k++) buf[k] = noise();
        for (uint32_t k = 0; k < ntx && s0 + (int)k < n; k++) buf[s0 + k] += tx[k] / 32768.0f * 0.1f;
        ftx_worker_reset();
        found = 0;
        for (int p = 0; p + bs <= n && !ftx_worker_is_full(); p += bs) {
            ftx_worker_put_rx_samples(buf + p, bs);
            ftx_worker_decode(lag_cb, false, NULL);
        }
        ftx_worker_decode(lag_cb, true, NULL);
        if (!found) continue;
        decoded++;

        float late = (found_t - start) / symbol;    /* symbols */

        sum += late;
        if (fabsf(late - FTX_SYNC_DECODER_LAG) > worst) worst = fabsf(late - FTX_SYNC_DECODER_LAG);
    }
    free(buf);
    free(tx);
    ftx_worker_free();

    if (decoded)
        printf("  %s: the decoder's time late by %.3f symbols on average (%.3f s), at most %.3f symbols off that\n",
               ftx_protocol_name(proto), sum / decoded, sum / decoded * symbol, worst);
    snprintf(what, sizeof(what), "%s: all 64 decoded (%d)", ftx_protocol_name(proto), decoded);
    check(decoded == 64, what);
    if (!decoded) return;
    snprintf(what, sizeof(what), "%s: late by %.3f symbols on average (%.2f s)", ftx_protocol_name(proto),
             sum / decoded, sum / decoded * symbol);
    check(fabsf(sum / decoded - FTX_SYNC_DECODER_LAG) <= 0.1f, what);
    snprintf(what, sizeof(what), "%s: each within 0.6 symbol of the lag (%.3f)", ftx_protocol_name(proto), worst);
    check(worst <= 0.6f, what);
}

/* ---- 2. the decoder thread in real time ---- */

#define LATE_S      0.3f    /* the signal's DT, every slot */
#define FRAGMENT    1103    /* 0.1 s */
#define MAX_HEARD   16

static struct {
    pthread_mutex_t mux;
    int             n;
    float           dt[MAX_HEARD], into[MAX_HEARD], time_sec[MAX_HEARD];
} heard = {.mux = PTHREAD_MUTEX_INITIALIZER};

static int16_t        *ft4_tx;
static uint32_t        ft4_ntx;
static int64_t         base_s;      /* a minute's start: sample n is base_s + n / RATE */
static atomic_bool     feeding;
static audio_worker_t *worker;

static void on_message(const char *text, int snr, float freq_hz, float time_sec, const slot_info_t *info,
                       void *ctx) {
    (void)text, (void)snr, (void)freq_hz, (void)ctx;
    pthread_mutex_lock(&heard.mux);
    if (heard.n < MAX_HEARD && info->rx_start_ns) {
        int64_t s   = info->rx_start_ns;
        int64_t off = ftx_slot_offset_ns(s / NS, (long)(s % NS), ftx_slot_time(FT4));

        heard.dt[heard.n]       = ftx_sync_dt(FT4, info->rx_start_ns, time_sec);
        heard.into[heard.n]     = (float)off / 1e9f;
        heard.time_sec[heard.n] = time_sec;
        heard.n++;
    }
    pthread_mutex_unlock(&heard.mux);
}

/* Every other slot the thread is kept past the slot's end */
static void on_tick(const slot_info_t *info, bool new_slot, float sec, void *ctx) {
    (void)ctx;
    if (!new_slot && info->odd && sec > ftx_slot_time(FT4) - 0.2f) usleep(500000);
}

/* PulseAudio's fragments, each fed as its last sample comes: the signal LATE_S after 0.5 s into each slot */
static void *feeder(void *arg) {
    (void)arg;
    struct timespec now;
    float           frag[FRAGMENT];

    clock_gettime(CLOCK_REALTIME, &now);

    int64_t n = (int64_t)(now.tv_sec - base_s) * RATE + (int64_t)now.tv_nsec * RATE / NS;

    while (atomic_load(&feeding)) {
        for (int i = 0; i < FRAGMENT; i++) {
            int64_t k    = n + i;
            int64_t slot = (int64_t)floor(k / (ftx_slot_time(FT4) * RATE));
            int64_t at   = llround((slot * ftx_slot_time(FT4) + FTX_SYNC_START_S + LATE_S) * RATE);

            frag[i] = noise();
            if (k >= at && k - at < (int64_t)ft4_ntx) frag[i] += ft4_tx[k - at] / 32768.0f * 0.1f;
        }
        n += FRAGMENT;

        int64_t         end = (int64_t)base_s * NS + n * NS / RATE;   /* the fragment's last sample comes */
        struct timespec t   = {.tv_sec = end / NS, .tv_nsec = end % NS};

        while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &t, NULL)) {
        }
        audio_worker_feed(worker, FRAGMENT, frag);
    }
    return NULL;
}

static void decoder_thread(void) {
    audio_worker_cb_t cb = {.on_message = on_message, .on_tick = on_tick};
    struct timespec   now;
    pthread_t         th;
    char              what[128];

    worker = audio_worker_create(RATE, FTX_PROTOCOL_FT4, 200, 3000, &cb);
    check(worker != NULL, "the decoder thread made");
    if (!worker) return;
    /* Made before the thread starts: the encoder and the decoder share ft8_lib's call table */
    if (!ftx_worker_generate_tx_samples("CQ ON6URE JO21", 1500, RATE, &ft4_tx, &ft4_ntx)) {
        check(false, "the encoder makes the FT4 message");
        audio_worker_destroy(worker);
        return;
    }
    clock_gettime(CLOCK_REALTIME, &now);
    base_s = now.tv_sec - now.tv_sec % 60;
    atomic_store(&feeding, true);
    pthread_create(&th, NULL, feeder, NULL);
    audio_worker_start(worker);

    /* Until two slots of each kind decoded, 45 s at most */
    int early = 0, late = 0;

    for (int i = 0; i < 450 && (early < 2 || late < 2); i++) {
        usleep(100000);
        pthread_mutex_lock(&heard.mux);
        early = late = 0;
        for (int k = 0; k < heard.n; k++) {
            if (heard.into[k] < 0.2f) early++;
            if (heard.into[k] >= 0.25f) late++;
        }
        pthread_mutex_unlock(&heard.mux);
    }
    atomic_store(&feeding, false);
    pthread_join(th, NULL);
    audio_worker_destroy(worker);
    free(ft4_tx);

    float tmin = 99.0f, tmax = -99.0f;

    for (int k = 0; k < heard.n; k++) {
        printf("  audio from %+.3f s into its slot, the decoder's time %.3f s: DT %+.3f s\n", heard.into[k],
               heard.time_sec[k], heard.dt[k]);
        snprintf(what, sizeof(what), "audio from %+.2f s into its slot: DT %+.3f s, %.2f s within 0.05 s",
                 heard.into[k], heard.dt[k], LATE_S);
        check(fabsf(heard.dt[k] - LATE_S) <= 0.05f, what);
        if (heard.time_sec[k] < tmin) tmin = heard.time_sec[k];
        if (heard.time_sec[k] > tmax) tmax = heard.time_sec[k];
    }
    snprintf(what, sizeof(what), "slots whose audio started on time (%d) and late (%d), two each", early, late);
    check(early >= 2 && late >= 2, what);
    snprintf(what, sizeof(what), "the decoder's own time moved %.2f s with the audio's start", tmax - tmin);
    check(tmax - tmin >= 0.2f, what);
}

int main(void) {
    decoder_lag(FT8);
    decoder_lag(FT4);
    decoder_thread();
    printf("%d of %d right\n", cases - bad, cases);
    return bad != 0;
}
