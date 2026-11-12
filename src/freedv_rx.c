/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  FreeDV RADE V1, receive only. See freedv_rx.h for the threads.
 *
 *  The chain follows rade_c/src/rade_rx_wav.c: rade_nin() before every rade_rx(), the
 *  first five feature frames of every sync warm FARGAN up (fargan_cont), after that one
 *  fargan_synthesize() per 36-float feature frame gives 160 samples at 16 kHz.
 */

#include "freedv_rx.h"

#include <string.h>

#ifdef RADE_ENABLED

#include "audio.h"
#include "olivia_resamp.h"

#include "rade_api.h"
#include "rade_dsp.h"
#include "fargan.h"
#include "lpcnet.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MODEM_RATE      RADE_FS             /* 8000 */
#define SPEECH_RATE     RADE_FS_SPEECH      /* 16000 */
#define IN_SCALE        4.0f                /* float +-1.0 = int16 +-32768; rade_rx_wav: int16 * 2/16384 */
#define IN_RING         16384               /* 2 s of modem input, a power of two */
#define FEAT_RING       128                 /* feature frames of 10 ms: 1.28 s */
#define OUT_RING        65536               /* speech at 44100 Hz: 1.5 s */
#define PREBUF          5292                /* a talk burst starts playing at 120 ms */
#define PLAY_CHUNK      1024                /* audio_play() needs room for the whole write: keep it small */
#define WARMUP_FRAMES   5

/* One slot: a feature frame. FARGAN is not reset when the sync goes and comes back: rade_rx_wav
 * and other decoders both run on with the state they have (the warm-up is once per start) */
typedef struct {
    float feat[RADE_NB_TOTAL_FEATURES];
} feat_slot_t;

/* Single producer, single consumer rings: the writer owns head, the reader tail */
typedef struct {
    _Atomic uint32_t head, tail;
} spsc_t;

static struct {
    bool            up;
    struct rade    *rade;
    atomic_bool     stop;
    atomic_bool     accepting;          /* the audio thread may call put_audio */
    atomic_int      in_put;             /* ... and is inside it: stop waits for 0 */
    pthread_t       th_dec, th_synth, th_play;
    bool            th_live;

    float           in_ring[IN_RING];
    spsc_t          in_idx;
    olivia_resamp_t in_rs;              /* audio thread only */

    feat_slot_t     feat_ring[FEAT_RING];
    spsc_t          feat_idx;

    int16_t         out_ring[OUT_RING];
    spsc_t          out_idx;
    olivia_resamp_t out_rs;             /* synth thread only */
    olivia_resamp_t tap_rs;             /* synth thread only: 16 kHz to the tap's rate */
    freedv_rx_tap_t tap;                /* set while stopped */

    pthread_mutex_t lock;               /* st */
    freedv_rx_stats_t st;
    _Atomic int     peak_x1000;         /* since the last decode round, audio thread writes */
} rx;

static uint32_t sp_used(const spsc_t *s)
{
    return atomic_load_explicit(&s->head, memory_order_acquire) - atomic_load_explicit(&s->tail, memory_order_acquire);
}

static int ms_since(const struct timespec *t0)
{
    struct timespec t1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (int)((t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

static void stat_max(int *field, int v)
{
    pthread_mutex_lock(&rx.lock);
    if (v > *field)
        *field = v;
    pthread_mutex_unlock(&rx.lock);
}

static void stat_inc(uint32_t *field, uint32_t n)
{
    pthread_mutex_lock(&rx.lock);
    *field += n;
    pthread_mutex_unlock(&rx.lock);
}

/* 3 to 8 of A-Z, 0-9 and /, with a letter and a digit in them */
static bool callsign_like(const char *s, int len)
{
    bool letter = false, digit = false;

    if (len < 3 || len > 8)
        return false;
    for (int i = 0; i < len; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z')
            letter = true;
        else if (s[i] >= '0' && s[i] <= '9')
            digit = true;
        else if (s[i] != '/')
            return false;
    }
    return letter && digit;
}

static void feat_push(const float *feat)
{
    uint32_t h = atomic_load_explicit(&rx.feat_idx.head, memory_order_relaxed);

    if (sp_used(&rx.feat_idx) >= FEAT_RING) {
        /* The synth thread is behind: the speech would only get later. The frame is lost */
        stat_inc(&rx.st.speech_dropped, LPCNET_FRAME_SIZE);
        return;
    }
    feat_slot_t *s = &rx.feat_ring[h % FEAT_RING];

    memcpy(s->feat, feat, sizeof(s->feat));
    atomic_store_explicit(&rx.feat_idx.head, h + 1, memory_order_release);
}

/* Decode thread: 8 kHz samples to speech features */
static void *decode_thread(void *arg)
{
    (void)arg;
    int         nin_max = rade_nin_max(rx.rade);
    int         n_feat  = rade_n_features_in_out(rx.rade);
    int         n_eoo   = rade_n_eoo_bits(rx.rade);
    RADE_COMP  *in      = malloc(sizeof(RADE_COMP) * nin_max);
    float      *feat    = malloc(sizeof(float) * n_feat);
    float      *eoo     = n_eoo ? malloc(sizeof(float) * n_eoo) : NULL;

    if (!in || !feat || (n_eoo && !eoo)) {
        free(in); free(feat); free(eoo);
        return NULL;
    }
    while (!atomic_load(&rx.stop)) {
        int nin = rade_nin(rx.rade);

        if (sp_used(&rx.in_idx) < (uint32_t)nin) {
            usleep(4000);
            continue;
        }
        uint32_t t = atomic_load_explicit(&rx.in_idx.tail, memory_order_relaxed);

        for (int i = 0; i < nin; i++) {
            in[i].real = rx.in_ring[(t + i) % IN_RING];
            in[i].imag = 0.0f;      /* real signal: the OFDM correlators reject the mirror, no Hilbert */
        }
        atomic_store_explicit(&rx.in_idx.tail, t + nin, memory_order_release);

        struct timespec t0;
        int             has_eoo = 0;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        int  n_out = rade_rx(rx.rade, feat, &has_eoo, eoo, in);
        int  ms = ms_since(&t0);
        bool sync = rade_sync(rx.rade) != 0;
        int  peak = atomic_exchange(&rx.peak_x1000, 0);

        pthread_mutex_lock(&rx.lock);
        if (ms > rx.st.decode_ms_max)
            rx.st.decode_ms_max = ms;
        rx.st.sync = sync;
        rx.st.snr_db = sync ? rade_snrdB_3k_est(rx.rade) : 0.0f;
        rx.st.foff_hz = sync ? rade_freq_offset(rx.rade) : 0.0f;
        if (peak > 0 || rx.st.level_pct > 0) {
            /* a round is 120 ms: one value per round, the bar falls on its own */
            int pct = peak / 10;
            rx.st.level_pct = pct > 100 ? 100 : pct;
        }
        if (has_eoo) {
            rx.st.eoo_count++;
            /* 8 characters of 7 bits, the soft bit above 0 is a 1 (rade_v1_text_test.c) */
            if (eoo && n_eoo >= 8 * 7) {
                char call[9];
                int  len = 8;

                for (int i = 0; i < 8; i++) {
                    unsigned c = 0;

                    for (int b = 0; b < 7; b++)
                        c |= (unsigned)(eoo[i * 7 + b] > 0.0f) << (6 - b);
                    call[i] = (c >= 32 && c < 127) ? (char)c : ' ';
                }
                while (len > 0 && call[len - 1] == ' ')
                    len--;
                /* Only what reads as a callsign: FreeDV's own program sends its callsign in another form (with
                   error correction), which came out here as garbage (" 7i:Iw" off the air) */
                if (!callsign_like(call, len))
                    len = 0;
                memcpy(rx.st.callsign, call, (size_t)len);
                rx.st.callsign[len] = '\0';
            }
        }
        pthread_mutex_unlock(&rx.lock);

        if (n_out > 0) {
            int frames = n_out / RADE_NB_TOTAL_FEATURES;

            for (int f = 0; f < frames; f++)
                feat_push(&feat[f * RADE_NB_TOTAL_FEATURES]);
        }
    }
    free(in); free(feat); free(eoo);
    return NULL;
}

/* Synth thread: features to 44100 Hz speech */
static void *synth_thread(void *arg)
{
    (void)arg;
    FARGANState fargan;
    bool        ready = false;
    int         cont_frames = 0;
    float       cont_buf[WARMUP_FRAMES * RADE_NB_TOTAL_FEATURES];
    int         out_max = olivia_resamp_max_out(&rx.out_rs, LPCNET_FRAME_SIZE);
    float      *res = malloc(sizeof(float) * out_max);
    int         tap_max = rx.tap ? olivia_resamp_max_out(&rx.tap_rs, LPCNET_FRAME_SIZE) : 0;
    float      *tres = tap_max ? malloc(sizeof(float) * tap_max) : NULL;
    int         batch = 0, batch_ms = 0;
    struct timespec t0;

    if (!res || (tap_max && !tres)) {
        free(res); free(tres);
        return NULL;
    }
    fargan_init(&fargan);

    while (!atomic_load(&rx.stop)) {
        if (!sp_used(&rx.feat_idx)) {
            usleep(3000);
            continue;
        }
        uint32_t           t = atomic_load_explicit(&rx.feat_idx.tail, memory_order_relaxed);
        const feat_slot_t *s = &rx.feat_ring[t % FEAT_RING];

        if (!ready) {
            /* rade_rx_wav: the first five frames are only for fargan_cont() */
            memcpy(&cont_buf[cont_frames * RADE_NB_TOTAL_FEATURES], s->feat, sizeof(s->feat));
            if (++cont_frames >= WARMUP_FRAMES) {
                float packed[WARMUP_FRAMES * NB_FEATURES];
                float zeros[FARGAN_CONT_SAMPLES];

                for (int i = 0; i < WARMUP_FRAMES; i++)
                    memcpy(&packed[i * NB_FEATURES], &cont_buf[i * RADE_NB_TOTAL_FEATURES],
                           sizeof(float) * NB_FEATURES);
                memset(zeros, 0, sizeof(zeros));
                fargan_cont(&fargan, zeros, packed);
                ready = true;
            }
            atomic_store_explicit(&rx.feat_idx.tail, t + 1, memory_order_release);
            continue;
        }

        float fpcm[LPCNET_FRAME_SIZE];

        clock_gettime(CLOCK_MONOTONIC, &t0);
        fargan_synthesize(&fargan, fpcm, s->feat);
        batch_ms += ms_since(&t0);
        atomic_store_explicit(&rx.feat_idx.tail, t + 1, memory_order_release);

        if (tres) {
            int m = olivia_resamp_process(&rx.tap_rs, fpcm, LPCNET_FRAME_SIZE, tres);

            if (m > 0)
                rx.tap(tres, (size_t)m);
        }

        int n = olivia_resamp_process(&rx.out_rs, fpcm, LPCNET_FRAME_SIZE, res);
        uint32_t h = atomic_load_explicit(&rx.out_idx.head, memory_order_relaxed);

        if (OUT_RING - sp_used(&rx.out_idx) < (uint32_t)n) {
            stat_inc(&rx.st.speech_dropped, n);
        } else {
            for (int i = 0; i < n; i++) {
                float v = res[i] * 32767.0f;

                if (v > 32767.0f) v = 32767.0f;
                if (v < -32767.0f) v = -32767.0f;
                rx.out_ring[(h + i) % OUT_RING] = (int16_t)lrintf(v);
            }
            atomic_store_explicit(&rx.out_idx.head, h + n, memory_order_release);
        }
        /* The budget is per modem frame: four 10 ms frames in under 120 ms (the synthesis only,
         * not the wait for the next frame) */
        if (++batch == 4) {
            stat_max(&rx.st.synth_ms_max, batch_ms);
            batch = batch_ms = 0;
        }
    }
    free(res);
    free(tres);
    return NULL;
}

/* Play thread: the speech to the Bluetooth headphones. Variant A only: without them the
 * speech is thrown away (the base gives the speaker no app audio while it decodes) */
static void *play_thread(void *arg)
{
    (void)arg;
    int16_t buf[PLAY_CHUNK];
    bool    bt = false, playing = false;
    struct timespec burst_t0;
    long    burst_written = 0;      /* samples given to the stream since the burst began */

    while (!atomic_load(&rx.stop)) {
        uint32_t used = sp_used(&rx.out_idx);

        if (!bt) {
            bt = audio_play_bt_begin();
            pthread_mutex_lock(&rx.lock);
            rx.st.audio_out = bt;
            pthread_mutex_unlock(&rx.lock);
            if (!bt) {
                /* nobody to play to: keep the ring empty, look again in a second */
                atomic_store(&rx.out_idx.tail, atomic_load(&rx.out_idx.head));
                for (int i = 0; i < 100 && !atomic_load(&rx.stop); i++)
                    usleep(10000);
                continue;
            }
        }
        if (!playing) {
            if (used < PREBUF) {
                usleep(5000);
                continue;
            }
            playing = true;
            burst_written = 0;
            clock_gettime(CLOCK_MONOTONIC, &burst_t0);
        }
        if (!used) {
            /* The ring is empty, the stream may not be: it ran dry only when the audio given to
             * it is shorter than the time that has passed since the burst began */
            if (burst_written * 1000 / AUDIO_PLAY_RATE < ms_since(&burst_t0)) {
                playing = false;
                stat_inc(&rx.st.underruns, 1);
            } else {
                usleep(2000);
            }
            continue;
        }
        uint32_t t = atomic_load_explicit(&rx.out_idx.tail, memory_order_relaxed);
        uint32_t n = used < PLAY_CHUNK ? used : PLAY_CHUNK;

        for (uint32_t i = 0; i < n; i++)
            buf[i] = rx.out_ring[(t + i) % OUT_RING];
        atomic_store_explicit(&rx.out_idx.tail, t + n, memory_order_release);
        burst_written += n;
        if (audio_play(buf, n) < 0) {
            audio_play_bt_end();
            bt = false;
            playing = false;
        }
    }
    if (bt) {
        audio_play_flush();
        audio_play_bt_end();
    }
    return NULL;
}

bool freedv_rx_available(void) { return true; }

bool freedv_rx_start(void)
{
    static bool first = true;

    if (rx.up)
        return true;
    if (first) {
        pthread_mutex_init(&rx.lock, NULL);
        rade_initialize();
        first = false;
    }
    memset(&rx.st, 0, sizeof(rx.st));
    atomic_store(&rx.in_idx.head, 0);   atomic_store(&rx.in_idx.tail, 0);
    atomic_store(&rx.feat_idx.head, 0); atomic_store(&rx.feat_idx.tail, 0);
    atomic_store(&rx.out_idx.head, 0);  atomic_store(&rx.out_idx.tail, 0);
    atomic_store(&rx.peak_x1000, 0);
    atomic_store(&rx.stop, false);

    if (olivia_resamp_init(&rx.in_rs, FREEDV_RX_IN_RATE, MODEM_RATE) != 0)
        return false;
    if (olivia_resamp_init(&rx.out_rs, SPEECH_RATE, AUDIO_PLAY_RATE) != 0) {
        olivia_resamp_free(&rx.in_rs);
        return false;
    }
    if (rx.tap && olivia_resamp_init(&rx.tap_rs, SPEECH_RATE, FREEDV_RX_TAP_RATE) != 0) {
        olivia_resamp_free(&rx.in_rs);
        olivia_resamp_free(&rx.out_rs);
        return false;
    }
    rx.st.tap_out = rx.tap != NULL;
    /* V1; the weights are compiled in, the file name is not used */
    rx.rade = rade_open("", RADE_VERBOSE_0);
    if (!rx.rade) {
        olivia_resamp_free(&rx.in_rs);
        olivia_resamp_free(&rx.out_rs);
        if (rx.tap)
            olivia_resamp_free(&rx.tap_rs);
        return false;
    }
    if (pthread_create(&rx.th_dec, NULL, decode_thread, NULL) != 0 ||
        pthread_create(&rx.th_synth, NULL, synth_thread, NULL) != 0 ||
        pthread_create(&rx.th_play, NULL, play_thread, NULL) != 0) {
        /* the ones that started are joined by stop; the flag is set first */
        rx.up = true;
        freedv_rx_stop();
        return false;
    }
    rx.th_live = true;
    rx.up = true;
    rx.st.running = true;
    atomic_store(&rx.accepting, true);
    return true;
}

void freedv_rx_stop(void)
{
    if (!rx.up)
        return;
    atomic_store(&rx.accepting, false);
    while (atomic_load(&rx.in_put))     /* the audio thread leaves put_audio before its resampler goes */
        usleep(500);
    atomic_store(&rx.stop, true);
    if (rx.th_live) {
        pthread_join(rx.th_dec, NULL);
        pthread_join(rx.th_synth, NULL);
        pthread_join(rx.th_play, NULL);
        rx.th_live = false;
    }
    rade_close(rx.rade);
    rx.rade = NULL;
    olivia_resamp_free(&rx.in_rs);
    olivia_resamp_free(&rx.out_rs);
    if (rx.tap)
        olivia_resamp_free(&rx.tap_rs);
    rx.up = false;
    pthread_mutex_lock(&rx.lock);
    rx.st.running = false;
    pthread_mutex_unlock(&rx.lock);
}

static void put_audio(unsigned int count, const float *samples)
{
    float    tmp[2048];
    float    res[2048];
    int      peak = 0;

    while (count) {
        unsigned n = count < 2048 ? count : 2048;

        for (unsigned i = 0; i < n; i++) {
            int p = (int)(fabsf(samples[i]) * 1000.0f);

            if (p > peak) peak = p;
            tmp[i] = samples[i] * IN_SCALE;
        }
        int m = olivia_resamp_process(&rx.in_rs, tmp, (int)n, res);
        uint32_t h = atomic_load_explicit(&rx.in_idx.head, memory_order_relaxed);

        if (IN_RING - sp_used(&rx.in_idx) < (uint32_t)m) {
            stat_inc(&rx.st.in_dropped, (uint32_t)m);
        } else {
            for (int i = 0; i < m; i++)
                rx.in_ring[(h + i) % IN_RING] = res[i];
            atomic_store_explicit(&rx.in_idx.head, h + m, memory_order_release);
        }
        samples += n;
        count -= n;
    }
    int old = atomic_load(&rx.peak_x1000);

    if (peak > old)
        atomic_store(&rx.peak_x1000, peak);
}

void freedv_rx_put_audio(unsigned int count, const float *samples)
{
    if (!atomic_load(&rx.accepting))
        return;
    atomic_fetch_add(&rx.in_put, 1);
    if (atomic_load(&rx.accepting))     /* stop may have come between the two */
        put_audio(count, samples);
    atomic_fetch_sub(&rx.in_put, 1);
}

void freedv_rx_set_tap(freedv_rx_tap_t tap)
{
    if (!rx.up)
        rx.tap = tap;
}

void freedv_rx_get_stats(freedv_rx_stats_t *out)
{
    if (!rx.up) {
        memset(out, 0, sizeof(*out));
        return;
    }
    pthread_mutex_lock(&rx.lock);
    *out = rx.st;
    pthread_mutex_unlock(&rx.lock);
}

#else   /* RADE_ENABLED */

bool freedv_rx_available(void) { return false; }
bool freedv_rx_start(void) { return false; }
void freedv_rx_stop(void) {}
void freedv_rx_put_audio(unsigned int count, const float *samples) { (void)count; (void)samples; }
void freedv_rx_set_tap(freedv_rx_tap_t tap) { (void)tap; }
void freedv_rx_get_stats(freedv_rx_stats_t *out) { memset(out, 0, sizeof(*out)); }

#endif
