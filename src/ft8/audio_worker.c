/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 audio / decode worker
 *
 *  Copyright (c) 2026
 */

#include "audio_worker.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <liquid/liquid.h>
#include "util.h"
#include <ft8lib/constants.h>

#include "worker.h"
#include "ft2.h"

/* Waterfall display width in pixels; used to size the FFT so each pixel maps
 * to one bin over the visible band. Matches dialog_ft8.c. */
#define WORKER_DISPLAY_WIDTH   771

struct audio_worker_s {
    /* Configuration (const after create). */
    int            sample_rate;       /* audio_sample_rate */
    ftx_protocol_t proto;
    int32_t        filter_low_hz;
    int32_t        filter_high_hz;
    uint16_t       nfft;
    uint16_t       fps_ms;            /* min interval between PSD emissions */

    audio_worker_cb_t cb;

    /* Thread lifecycle. joinable is the caller's (start/stop/destroy), not the thread's: set once
     * pthread_create() succeeds, the thread joined while it is, run yet or not (audit C04) */
    pthread_t       thread;
    bool            joinable;
    atomic_bool     stop_req;
    pthread_mutex_t sleep_mux;
    pthread_cond_t  sleep_cv;

    /* Audio buffer (written by audio_worker_feed, read by worker). */
    pthread_mutex_t audio_mux;
    cbufferf        audio_buf;

    /* When the decoder's audio began (Auto Time Sync, time_sync.h), under audio_mux: the buffer emptied (created,
     * drained) marks the next feed as the first sample's, which is stamped as it comes, by the clocks of then less
     * the fragment's length (it is the fragment's first sample). A feed that does not fit loses samples: the time
     * of what follows is not known until the next drain */
    bool            stamp_next;
    bool            stamp_lost;
    int64_t         stamp_ns, stamp_mono_ns;

    /* DSP state (worker thread only). */
    spgramf         sg;
    float          *psd;
    float complex  *audio_frame_buf;
    size_t          audio_frame_buf_len;
    int             block_size;

    uint64_t        last_psd_time_ms;

    /* The decoder's audio (worker thread): when its first sample came, 0 not known; the blocks put into it since;
     * wf_first: the next block read is its first */
    int64_t         wf_ns, wf_mono_ns;
    uint32_t        wf_blocks;
    bool            wf_first;
};

/* ---------- small helpers ---------------------------------------------- */

static bool get_time_slot(ftx_protocol_t proto, struct timespec now, float *sec_since_start) {
    float sec = (now.tv_sec % 60) + now.tv_nsec / 1.0e9f;
    float slot_time = ftx_slot_time(proto);
    *sec_since_start = fmodf(sec, slot_time);
    return ((int)(sec / slot_time) % 2) != 0;
}

static void msleep_interruptible(audio_worker_t *w, long ms) {
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec  += ms / 1000;
    abstime.tv_nsec += (ms % 1000) * 1000000L;
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec  += 1;
        abstime.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&w->sleep_mux);
    if (!atomic_load(&w->stop_req)) {
        pthread_cond_timedwait(&w->sleep_cv, &w->sleep_mux, &abstime);
    }
    pthread_mutex_unlock(&w->sleep_mux);
}

static int64_t clock_ns(clockid_t id) {
    struct timespec t;

    clock_gettime(id, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* The decoder's audio starts again with the next sample fed */
static void drain_audio_buf(audio_worker_t *w) {
    pthread_mutex_lock(&w->audio_mux);
    if (w->audio_buf) {
        cbufferf_reset(w->audio_buf);
    }
    w->stamp_next = true;
    w->stamp_lost = false;
    pthread_mutex_unlock(&w->audio_mux);
    w->wf_ns = w->wf_mono_ns = 0;
    w->wf_blocks = 0;
    w->wf_first  = true;
}

/* ---------- PSD / waterfall ------------------------------------------- */

static void maybe_emit_psd(audio_worker_t *w, const slot_info_t *info,
                           float sec_since_slot_start) {
    uint64_t now = get_time();
    if ((now - w->last_psd_time_ms) < w->fps_ms) return;
    w->last_psd_time_ms = now;

    spgramf_get_psd(w->sg, w->psd);
    liquid_vectorf_addscalar(w->psd, w->nfft,
                             -10.f * log10f(sqrtf(w->nfft)),
                             w->psd);

    if (w->cb.on_psd) {
        w->cb.on_psd(w->psd, w->nfft, sec_since_slot_start, info, w->cb.ctx);
    }

    spgramf_reset(w->sg);
}

/* ---------- ftx_worker decode callback trampoline --------------------- */

typedef struct {
    audio_worker_t    *w;
    const slot_info_t *info;
} decode_ctx_t;

static void decode_cb(const char *text, int snr, float freq_hz, float time_sec, void *user) {
    decode_ctx_t *dc = (decode_ctx_t *)user;
    if (dc && dc->w && dc->w->cb.on_message) {
        slot_info_t info = *dc->info;

        info.rx_start_ns      = dc->w->wf_ns;
        info.rx_start_mono_ns = dc->w->wf_mono_ns;
        dc->w->cb.on_message(text, snr, freq_hz, time_sec, &info, dc->w->cb.ctx);
    }
}

/* ---------- RX frame pump --------------------------------------------- */

static void rx_consume_frames(audio_worker_t *w, const slot_info_t *info,
                              float sec_since_slot_start) {
    decode_ctx_t dc = {.w = w, .info = info};

    for (;;) {
        if (atomic_load(&w->stop_req)) return;

        unsigned int   got       = 0;
        float         *block       = NULL;
        float          block_data[w->block_size];

        pthread_mutex_lock(&w->audio_mux);
        if (cbufferf_size(w->audio_buf) >= w->block_size) {
            cbufferf_read(w->audio_buf, w->block_size, &block, &got);
            // Copy data to external buffer
            memcpy((void*)block_data, (void*)block, got * sizeof(float));
            block = block_data;
            cbufferf_release(w->audio_buf, got);
            /* The first block since the drain: it begins with the sample stamped */
            if (w->wf_first) {
                w->wf_first   = false;
                w->wf_ns      = w->stamp_next ? 0 : w->stamp_ns;
                w->wf_mono_ns = w->stamp_next ? 0 : w->stamp_mono_ns;
            }
            if (w->stamp_lost) w->wf_ns = w->wf_mono_ns = 0;
        }
        pthread_mutex_unlock(&w->audio_mux);

        if (!block) {
            break;
        }

        spgramf_write(w->sg, block, w->block_size);
        maybe_emit_psd(w, info, sec_since_slot_start);

        ftx_worker_put_rx_samples(block, w->block_size);
        w->wf_blocks++;

        if (ftx_worker_is_full()) {
            ftx_worker_decode(decode_cb, true, (void *)&dc);
            ftx_worker_reset();
            /* The decoder's audio goes on from the next block, this many samples on */
            if (w->wf_ns) {
                int64_t on = (int64_t)w->wf_blocks * w->block_size * 1000000000LL / w->sample_rate;

                w->wf_ns      += on;
                w->wf_mono_ns += on;
            }
            w->wf_blocks = 0;
        } else {
            ftx_worker_decode(decode_cb, false, (void *)&dc);
        }
    }
}

/* ---------- thread body ---------------------------------------------- */

static void *worker_main(void *arg) {
    audio_worker_t *w = (audio_worker_t *)arg;
    slot_info_t     info = { .odd = false, .answer_generated = false };
    decode_ctx_t    dc   = { .w = w, .info = &info };

    while (!atomic_load(&w->stop_req)) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);

        float sec_since_slot_start = 0.0f;
        bool  new_odd  = get_time_slot(w->proto, now, &sec_since_slot_start);
        bool  new_slot = (new_odd != info.odd);
        bool  tick_done = false;

        /* FT2: a transmission must start within 0.8 s of its slot and lasts
         * 2.52 s, but the last decode of the slot just ended takes up to
         * 0.5 s on the radio. So on_tick (which starts a pending TX) runs
         * first, the last decode after it (SP2KMO's upstream 76d98ce2). */
        if (new_slot && (int)w->proto == (int)FTX_PROTOCOL_FT2 && w->cb.on_tick) {
            slot_info_t next = info;

            next.odd = new_odd;
            w->cb.on_tick(&next, true, sec_since_slot_start, w->cb.ctx);
            tick_done = true;
        }

        rx_consume_frames(w, &info, sec_since_slot_start);

        if (new_slot) {
            /* Slot boundary: flush a final decode, drain stale audio that
             * accumulated during TX, and fire the slot_end handler. */
            ftx_worker_decode(decode_cb, true, (void *)&dc);
            ftx_worker_reset();
            drain_audio_buf(w);

            if (w->cb.on_slot_end) {
                w->cb.on_slot_end(&info, w->cb.ctx);
            }
            info.odd = new_odd;
        }

        if (w->cb.on_tick && !tick_done) {
            w->cb.on_tick(&info, new_slot, sec_since_slot_start, w->cb.ctx);
        }

        if (!new_slot && !atomic_load(&w->stop_req)) {
            msleep_interruptible(w, 100);
        }
    }
    return NULL;
}

/* ---------- public API ------------------------------------------------- */

audio_worker_t *audio_worker_create(int audio_sample_rate,
                                    ftx_protocol_t proto,
                                    int32_t filter_low_hz,
                                    int32_t filter_high_hz,
                                    const audio_worker_cb_t *cb) {
    if (audio_sample_rate <= 0) return NULL;
    int span = filter_high_hz - filter_low_hz;
    if (span <= 0) return NULL;

    audio_worker_t *w = (audio_worker_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->sample_rate    = audio_sample_rate;
    w->proto          = proto;
    w->filter_low_hz  = filter_low_hz;
    w->filter_high_hz = filter_high_hz;
    w->nfft           = (uint16_t)(WORKER_DISPLAY_WIDTH * w->sample_rate / span);
    w->fps_ms         = (uint16_t)(1000 / 2.5);   /* 2.5 Hz: the 80 px above the log show two slots */
    if (cb) w->cb = *cb;

    pthread_mutex_init(&w->audio_mux, NULL);
    pthread_mutex_init(&w->sleep_mux, NULL);
    pthread_cond_init(&w->sleep_cv, NULL);

    w->audio_buf = cbufferf_create(audio_sample_rate * 3);
    if (!w->audio_buf) goto fail;

    w->sg = spgramf_create(w->nfft, LIQUID_WINDOW_HANN, w->nfft, w->nfft / 2);
    if (!w->sg) goto fail;

    w->psd = (float *)malloc(w->nfft * sizeof(float));
    if (!w->psd) goto fail;

    ftx_worker_init(w->sample_rate, proto);
    w->block_size          = ftx_worker_get_block_size();
    if (w->block_size <= 0) goto fail;

    atomic_store(&w->stop_req, false);
    w->stamp_next = true;   /* the first sample fed begins the decoder's audio */
    w->wf_first   = true;

    return w;

fail:
    audio_worker_destroy(w);
    return NULL;
}

int audio_worker_start(audio_worker_t *w) {
    if (!w || w->joinable) return -1;   /* a second thread would take the first one's handle */
    atomic_store(&w->stop_req, false);
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        return -1;
    }
    w->joinable = true;
    return 0;
}

void audio_worker_stop(audio_worker_t *w) {
    if (!w) return;
    atomic_store(&w->stop_req, true);
    pthread_mutex_lock(&w->sleep_mux);
    pthread_cond_broadcast(&w->sleep_cv);
    pthread_mutex_unlock(&w->sleep_mux);

    /* Not "has it run yet": a thread not scheduled so far starts on a worker freed meanwhile */
    if (w->joinable) {
        pthread_join(w->thread, NULL);
        w->joinable = false;
    }
}

void audio_worker_destroy(audio_worker_t *w) {
    if (!w) return;
    audio_worker_stop(w);

    if (w->audio_frame_buf) {
        free(w->audio_frame_buf);
        w->audio_frame_buf = NULL;
    }
    if (w->psd) {
        free(w->psd);
        w->psd = NULL;
    }

    if (w->sg) {
        spgramf_destroy(w->sg);
        w->sg = NULL;
    }
    if (w->audio_buf) {
        cbufferf_destroy(w->audio_buf);
        w->audio_buf = NULL;
    }

    ftx_worker_free();

    pthread_mutex_destroy(&w->audio_mux);
    pthread_mutex_destroy(&w->sleep_mux);
    pthread_cond_destroy(&w->sleep_cv);

    free(w);
}

void audio_worker_feed(audio_worker_t *w, unsigned int n, float *samples) {
    if (!w || !samples || (n == 0)) return;

    /* The fragment's first sample came n samples before its last, which is now */
    int64_t back = (int64_t)n * 1000000000LL / w->sample_rate;
    int64_t now  = clock_ns(CLOCK_REALTIME), mono = clock_ns(CLOCK_MONOTONIC);

    pthread_mutex_lock(&w->audio_mux);
    if (w->audio_buf) {
        if (cbufferf_space_available(w->audio_buf) < n) w->stamp_lost = true;   /* cbufferf_write drops it all */
        else if (w->stamp_next) {
            w->stamp_ns      = now - back;
            w->stamp_mono_ns = mono - back;
            w->stamp_next    = false;
        }
        cbufferf_write(w->audio_buf, samples, n);
    }
    pthread_mutex_unlock(&w->audio_mux);
}

uint16_t audio_worker_nfft(const audio_worker_t *w) {
    return w ? w->nfft : 0;
}
