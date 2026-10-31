/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  FreeDV RADE V1 transmitter, see freedv_tx.h. The chain follows rade_c/src/rade_tx_wav.c:
 *  160 speech samples at 16 kHz -> lpcnet_compute_single_frame_features() -> 12 frames of 36
 *  floats -> rade_tx() -> 960 complex samples at 8 kHz -> real part * RADE_INT16_SCALE.
 */

#include "freedv_tx.h"

#include <string.h>

#ifdef RADE_ENABLED

#include "audio.h"
#include "olivia_resamp.h"

#include "rade_api.h"
#include "rade_dsp.h"
#include "lpcnet.h"
#include "arch.h"
#include "cpu_support.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define MODEM_RATE      RADE_FS             /* 8000 */
#define SPEECH_RATE     RADE_FS_SPEECH      /* 16000 */
#define MIC_RING        (1u << 20)          /* 16 kHz samples: 65 s, more than the longest over, so nothing is dropped */
#define STORE_SAMPLES   ((FREEDV_TX_MAX_S + 2) * MODEM_RATE + 4000)   /* the over at 8 kHz: 1.1 MB */
#define MAX_MIC_16K     (FREEDV_TX_MAX_S * SPEECH_RATE)

static struct {
    bool            up;                 /* begin() was called, discard() not yet */
    struct rade    *rade;
    LPCNetEncState *lpc;
    int             arch;
    pthread_t       th;
    bool            th_live;
    atomic_bool     abort;              /* the thread ends at once */
    atomic_bool     stop_req;           /* no more mic: finish what is in the ring, then the EOO */
    atomic_bool     accepting;          /* the audio thread may call put_mic */
    atomic_int      in_put;

    olivia_resamp_t mic_rs;             /* audio thread only */
    int16_t        *ring;
    _Atomic uint32_t head, tail;
    _Atomic uint32_t mic_16k;           /* speech samples that went into the ring */
    atomic_int      peak_x1000;

    int16_t        *store;              /* the over, 8 kHz, real part * 16384 */
    _Atomic uint32_t store_n;
    atomic_bool     done;
    atomic_int      encode_ms_max;
    _Atomic uint32_t encoded_16k;

    /* freedv_tx_read(): the store to 44100 Hz */
    olivia_resamp_t out_rs;
    bool            out_rs_ok;
    uint32_t        read_pos;
    bool            flushed;
    float           pend[8192];
    int             pend_n, pend_pos;
} tx;

static int ms_since(const struct timespec *t0)
{
    struct timespec t1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (int)((t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

static void store_real(const RADE_COMP *iq, int n)
{
    uint32_t at = atomic_load_explicit(&tx.store_n, memory_order_relaxed);

    if (at + (uint32_t)n > STORE_SAMPLES)
        return;     /* never: the mic stops at FREEDV_TX_MAX_S, and the store has room for that and the EOO */
    for (int i = 0; i < n; i++) {
        float v = iq[i].real * RADE_INT16_SCALE;

        if (v > 32767.0f) v = 32767.0f;
        if (v < -32767.0f) v = -32767.0f;
        tx.store[at + i] = (int16_t)lrintf(v);
    }
    atomic_store_explicit(&tx.store_n, at + n, memory_order_release);
}

static void *encoder_thread(void *arg)
{
    (void)arg;
    int         per_mf = rade_n_features_in_out(tx.rade) / RADE_NB_TOTAL_FEATURES;     /* 12 */
    int         n_tx = rade_n_tx_out(tx.rade);
    int         n_eoo = rade_n_tx_eoo_out(tx.rade);
    float      *feat = calloc((size_t)per_mf * RADE_NB_TOTAL_FEATURES, sizeof(float));
    RADE_COMP  *iq = malloc(sizeof(RADE_COMP) * (size_t)(n_tx > n_eoo ? n_tx : n_eoo));
    int         have = 0;
    struct timespec t0;

    if (!feat || !iq) {
        free(feat); free(iq);
        atomic_store(&tx.done, true);
        return NULL;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!atomic_load(&tx.abort)) {
        uint32_t t = atomic_load_explicit(&tx.tail, memory_order_relaxed);
        uint32_t used = atomic_load_explicit(&tx.head, memory_order_acquire) - t;

        if (used < LPCNET_FRAME_SIZE) {
            if (atomic_load(&tx.stop_req)) {
                /* the ring is final once stop_req is set: a piece shorter than 10 ms is dropped, as rade_tx_wav does */
                if (atomic_load_explicit(&tx.head, memory_order_acquire) - t < LPCNET_FRAME_SIZE)
                    break;
                continue;
            }
            usleep(3000);
            continue;
        }
        opus_int16 pcm[LPCNET_FRAME_SIZE];

        for (int i = 0; i < LPCNET_FRAME_SIZE; i++)
            pcm[i] = tx.ring[(t + i) & (MIC_RING - 1)];
        atomic_store_explicit(&tx.tail, t + LPCNET_FRAME_SIZE, memory_order_release);
        atomic_fetch_add(&tx.encoded_16k, LPCNET_FRAME_SIZE);

        lpcnet_compute_single_frame_features(tx.lpc, pcm, &feat[have * RADE_NB_TOTAL_FEATURES], tx.arch);
        if (++have >= per_mf) {
            int n = rade_tx(tx.rade, iq, feat);

            store_real(iq, n);
            have = 0;
            int ms = ms_since(&t0);

            if (ms > atomic_load(&tx.encode_ms_max))
                atomic_store(&tx.encode_ms_max, ms);
            clock_gettime(CLOCK_MONOTONIC, &t0);
        }
    }
    if (!atomic_load(&tx.abort)) {
        if (have > 0) {
            /* zero-fill the rest so the last speech goes out, as rade_tx_wav does */
            memset(&feat[have * RADE_NB_TOTAL_FEATURES], 0,
                   (size_t)(per_mf - have) * RADE_NB_TOTAL_FEATURES * sizeof(float));
            store_real(iq, rade_tx(tx.rade, iq, feat));
        }
        store_real(iq, rade_tx_eoo(tx.rade, iq));
    }
    free(feat); free(iq);
    atomic_store(&tx.done, true);
    return NULL;
}

/* The End-of-Over's 8 characters of 7 bits, +-1.0 (rade_v1_text_test.c) */
static void set_callsign(const char *call)
{
    int    n = rade_n_eoo_bits(tx.rade);
    float *bits = calloc((size_t)(n > 0 ? n : 1), sizeof(float));
    int    len = call ? (int)strlen(call) : 0;

    if (!bits || n < FREEDV_CALL_MAX * 7) {
        free(bits);
        return;
    }
    for (int i = 0; i < FREEDV_CALL_MAX; i++) {
        unsigned char c = i < len ? (unsigned char)toupper((unsigned char)call[i]) : ' ';

        if (c < 32 || c > 126)
            c = ' ';
        for (int b = 0; b < 7; b++)
            bits[i * 7 + b] = ((c >> (6 - b)) & 1) ? 1.0f : -1.0f;
    }
    rade_tx_set_eoo_bits(tx.rade, bits);
    free(bits);
}

bool freedv_tx_available(void) { return true; }

bool freedv_tx_begin(const char *callsign)
{
    static bool first = true;

    if (tx.up)
        return false;
    if (first) {
        rade_initialize();      /* rx or tx, the first of them; once */
        first = false;
    }
    memset(&tx, 0, sizeof(tx));
    tx.ring = malloc(sizeof(int16_t) * MIC_RING);
    tx.store = malloc(sizeof(int16_t) * STORE_SAMPLES);
    if (!tx.ring || !tx.store || olivia_resamp_init(&tx.mic_rs, FREEDV_TX_MIC_RATE, SPEECH_RATE) != 0)
        goto fail;
    tx.rade = rade_open("", RADE_VERBOSE_0);
    tx.lpc = lpcnet_encoder_create();
    tx.arch = opus_select_arch();
    if (!tx.rade || !tx.lpc) {
        olivia_resamp_free(&tx.mic_rs);
        goto fail;
    }
    set_callsign(callsign);
    if (pthread_create(&tx.th, NULL, encoder_thread, NULL) != 0) {
        olivia_resamp_free(&tx.mic_rs);
        goto fail;
    }
    tx.th_live = true;
    tx.up = true;
    atomic_store(&tx.accepting, true);
    return true;
fail:
    if (tx.rade) rade_close(tx.rade);
    if (tx.lpc) lpcnet_encoder_destroy(tx.lpc);
    free(tx.ring);
    free(tx.store);
    memset(&tx, 0, sizeof(tx));
    return false;
}

static void put_mic(size_t count, const int16_t *samples)
{
    float tmp[2048], res[2048];
    int   peak = 0;

    while (count) {
        size_t n = count < 2048 ? count : 2048;

        for (size_t i = 0; i < n; i++) {
            int p = abs(samples[i]);

            if (p > peak) peak = p;
            tmp[i] = (float)samples[i];
        }
        int      m = olivia_resamp_process(&tx.mic_rs, tmp, (int)n, res);
        uint32_t h = atomic_load_explicit(&tx.head, memory_order_relaxed);

        if (atomic_load(&tx.mic_16k) + (uint32_t)m <= MAX_MIC_16K) {
            for (int i = 0; i < m; i++) {
                float v = res[i];

                if (v > 32767.0f) v = 32767.0f;
                if (v < -32767.0f) v = -32767.0f;
                tx.ring[(h + i) & (MIC_RING - 1)] = (int16_t)lrintf(v);
            }
            atomic_store_explicit(&tx.head, h + m, memory_order_release);
            atomic_fetch_add(&tx.mic_16k, (uint32_t)m);
        }
        samples += n;
        count -= n;
    }
    int old = atomic_load(&tx.peak_x1000);

    if (peak * 1000 / 32768 > old)
        atomic_store(&tx.peak_x1000, peak * 1000 / 32768);
}

void freedv_tx_put_mic(size_t count, const int16_t *samples)
{
    if (!atomic_load(&tx.accepting))
        return;
    atomic_fetch_add(&tx.in_put, 1);
    if (atomic_load(&tx.accepting))
        put_mic(count, samples);
    atomic_fetch_sub(&tx.in_put, 1);
}

void freedv_tx_stop(void)
{
    if (!tx.up || atomic_load(&tx.stop_req))
        return;
    atomic_store(&tx.accepting, false);
    while (atomic_load(&tx.in_put))
        usleep(500);
    atomic_store(&tx.stop_req, true);       /* the ring does not grow any more */
}

void freedv_tx_discard(void)
{
    if (!tx.up)
        return;
    atomic_store(&tx.accepting, false);
    while (atomic_load(&tx.in_put))
        usleep(500);
    atomic_store(&tx.abort, true);
    if (tx.th_live)
        pthread_join(tx.th, NULL);
    rade_close(tx.rade);
    lpcnet_encoder_destroy(tx.lpc);
    olivia_resamp_free(&tx.mic_rs);
    if (tx.out_rs_ok)
        olivia_resamp_free(&tx.out_rs);
    free(tx.ring);
    free(tx.store);
    memset(&tx, 0, sizeof(tx));
}

void freedv_tx_rewind(void)
{
    if (!tx.up)
        return;
    if (tx.out_rs_ok)
        olivia_resamp_reset(&tx.out_rs);
    tx.read_pos = 0;
    tx.flushed = false;
    tx.pend_n = tx.pend_pos = 0;
}

/* tx_fill_fn_t: the over from the store at 8 kHz to the 44100 Hz of audio_play() */
size_t freedv_tx_read(int16_t *buf, size_t n, void *ctx)
{
    (void)ctx;
    if (!tx.up || !atomic_load(&tx.done))
        return 0;
    if (!tx.out_rs_ok) {
        if (olivia_resamp_init(&tx.out_rs, MODEM_RATE, AUDIO_PLAY_RATE) != 0)
            return 0;
        tx.out_rs_ok = true;
    }
    uint32_t total = atomic_load(&tx.store_n);
    size_t   got = 0;

    while (got < n) {
        if (tx.pend_pos < tx.pend_n) {
            int take = tx.pend_n - tx.pend_pos;

            if ((size_t)take > n - got)
                take = (int)(n - got);
            for (int i = 0; i < take; i++) {
                float v = tx.pend[tx.pend_pos + i];

                if (v > 32767.0f) v = 32767.0f;
                if (v < -32767.0f) v = -32767.0f;
                buf[got + i] = (int16_t)lrintf(v);
            }
            tx.pend_pos += take;
            got += (size_t)take;
            continue;
        }
        float in[256];
        int   m = 0;

        tx.pend_n = tx.pend_pos = 0;
        while (m < 256 && tx.read_pos < total)
            in[m++] = tx.store[tx.read_pos++];
        if (m == 0) {
            if (tx.flushed)
                break;
            /* the resampler holds back half its kernel: zeros push the end of the over out */
            for (; m < OLIVIA_RESAMP_TAPS; m++)
                in[m] = 0.0f;
            tx.flushed = true;
        }
        tx.pend_n = olivia_resamp_process(&tx.out_rs, in, m, tx.pend);
    }
    return got;
}

void freedv_tx_get_stats(freedv_tx_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!tx.up)
        return;
    uint32_t mic = atomic_load(&tx.mic_16k);

    out->active = !atomic_load(&tx.stop_req);
    out->full = mic >= (uint32_t)MAX_MIC_16K - SPEECH_RATE / 50;
    out->done = atomic_load(&tx.done);
    out->rec_ms = (int)((uint64_t)mic * 1000 / SPEECH_RATE);
    out->encoded_ms = (int)((uint64_t)atomic_load(&tx.encoded_16k) * 1000 / SPEECH_RATE);
    out->level_pct = atomic_exchange(&tx.peak_x1000, 0) / 10;
    out->encode_ms_max = atomic_load(&tx.encode_ms_max);
    out->over_ms = (int)((uint64_t)atomic_load(&tx.store_n) * 1000 / MODEM_RATE);
}

#else   /* RADE_ENABLED */

bool freedv_tx_available(void) { return false; }
bool freedv_tx_begin(const char *callsign) { (void)callsign; return false; }
void freedv_tx_put_mic(size_t count, const int16_t *samples) { (void)count; (void)samples; }
void freedv_tx_stop(void) {}
void freedv_tx_discard(void) {}
size_t freedv_tx_read(int16_t *buf, size_t n, void *ctx) { (void)buf; (void)n; (void)ctx; return 0; }
void freedv_tx_rewind(void) {}
void freedv_tx_get_stats(freedv_tx_stats_t *out) { memset(out, 0, sizeof(*out)); }

#endif
