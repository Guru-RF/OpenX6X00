/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - birdies (birdie.h): the list and its file, the scan, the notch.
 *
 *  The scan steps the receiver across every active band, a spectrum width at a time with the
 *  edges overlapping, and takes two spectra at each step. A narrow peak well over the noise
 *  that is in both is a carrier; with the antenna off it can only be the radio's own. The
 *  receiver's own centre spike moves with the VFO and is left out.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "birdie.h"

#include "cfg/cfg_api.h"
#include "dsp.h"
#include "radio.h"
#include "scheduler.h"
#include "spot_marks.h"

#include "lvgl/lvgl.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAME_HZ        150      /* two findings this close are one birdie */
#define OVER_FLOOR_DB  14.0f    /* a peak this much over the noise floor */
#define NARROW_DB      8.0f     /* and this much over its surroundings 4 bins away */
#define EDGE_BINS      24       /* the filter's slopes at the edges of the spectrum */
#define CENTRE_BINS    3        /* the receiver's own spike */
#define SETTLE_MS      400      /* after a step, before the first spectrum */
#define AGAIN_MS       250      /* between the two spectra */
#define NOTCH_MIN_HZ   100      /* the notch filter's range */
#define NOTCH_MAX_HZ   3000

static int32_t         birdies[BIRDIES_MAX];
static int             birdies_n;
static pthread_mutex_t list_mux = PTHREAD_MUTEX_INITIALIZER;

/* ---- the list ---------------------------------------------------------------------- */

static void list_sort(void) {
    for (int i = 1; i < birdies_n; i++) {
        int32_t v = birdies[i];
        int     j = i;
        while (j > 0 && birdies[j - 1] > v) {
            birdies[j] = birdies[j - 1];
            j--;
        }
        birdies[j] = v;
    }
}

/* Adds hz unless one within SAME_HZ is known; true when it is new */
static bool list_add(int32_t hz) {
    bool added = false;

    pthread_mutex_lock(&list_mux);
    int i;
    for (i = 0; i < birdies_n && abs(birdies[i] - hz) > SAME_HZ; i++) {}
    if (i == birdies_n && birdies_n < BIRDIES_MAX) {
        birdies[birdies_n++] = hz;
        list_sort();
        added = true;
    }
    pthread_mutex_unlock(&list_mux);
    return added;
}

static void list_load(void) {
    FILE *f = fopen(BIRDIE_FILE, "r");
    char  line[32];

    if (!f) return;
    pthread_mutex_lock(&list_mux);
    birdies_n = 0;
    while (birdies_n < BIRDIES_MAX && fgets(line, sizeof(line), f)) {
        long v = atol(line);
        if (v > 0) birdies[birdies_n++] = (int32_t)v;
    }
    list_sort();
    pthread_mutex_unlock(&list_mux);
    fclose(f);
}

static void list_save(void) {
    FILE *f = fopen(BIRDIE_FILE ".tmp", "w");

    if (!f) return;
    pthread_mutex_lock(&list_mux);
    for (int i = 0; i < birdies_n; i++) fprintf(f, "%d\n", (int)birdies[i]);
    pthread_mutex_unlock(&list_mux);
    fclose(f);
    rename(BIRDIE_FILE ".tmp", BIRDIE_FILE);
}

int birdie_count(void) {
    return birdies_n;
}

int32_t birdie_at(int i) {
    return i >= 0 && i < birdies_n ? birdies[i] : 0;
}

int birdie_count_in(int32_t lo, int32_t hi) {
    int n = 0;

    pthread_mutex_lock(&list_mux);
    for (int i = 0; i < birdies_n; i++)
        if (birdies[i] >= lo && birdies[i] <= hi) n++;
    pthread_mutex_unlock(&list_mux);
    return n;
}

void birdie_clear(void) {
    pthread_mutex_lock(&list_mux);
    birdies_n = 0;
    pthread_mutex_unlock(&list_mux);
    unlink(BIRDIE_FILE);
    spot_marks_refresh();
}

/* ---- the notch ----------------------------------------------------------------------- */

static bool    notch_ours;          /* DNF on because of a birdie: ours to take off again */
static int32_t notch_was_center;    /* the DNF centre before we moved it */
static int32_t notch_hz;            /* the birdie it is on */

static void notch_release(void) {
    if (!notch_ours) return;
    notch_ours = false;
    param_i_set(cfg_dnf, 0);
    param_i_set(cfg_dnf_center, notch_was_center);
}

/* The birdie in the passband, as an audio frequency: USB above the VFO, LSB below it. CW, AM
 * and FM are left alone: their passband sits elsewhere. 0 when none. */
static int32_t notch_wanted(void) {
    int32_t vfo  = cparam_i_get(cfg_fg_freq);
    int32_t mode = cparam_i_get(cfg_cur_mode);
    int32_t low  = cparam_i_get(cfg_cur_filter_low), high = cparam_i_get(cfg_cur_filter_high);
    bool    usb  = mode == radio_mode_usb || mode == radio_mode_usb_dig;
    bool    lsb  = mode == radio_mode_lsb || mode == radio_mode_lsb_dig;
    int32_t best = 0;

    if (!usb && !lsb) return 0;
    pthread_mutex_lock(&list_mux);
    for (int i = 0; i < birdies_n; i++) {
        int32_t audio = usb ? birdies[i] - vfo : vfo - birdies[i];

        if (audio < low || audio > high || audio < NOTCH_MIN_HZ || audio > NOTCH_MAX_HZ) continue;
        if (!best || abs(audio - 1000) < abs(best - 1000)) best = audio;   /* the one nearest the middle */
    }
    pthread_mutex_unlock(&list_mux);
    return best;
}

static void notch_apply(void) {
    int32_t want = param_i_get(cfg_birdie_marks) == 2 ? notch_wanted() : 0;

    if (!want) {
        notch_release();
        return;
    }
    if (notch_ours && want == notch_hz) return;
    if (!notch_ours) {
        if (param_i_get(cfg_dnf)) return;   /* the operator's own notch stays */
        notch_was_center = param_i_get(cfg_dnf_center);
        notch_ours       = true;
    }
    notch_hz = want;
    param_i_set(cfg_dnf_center, want);
    param_i_set(cfg_dnf, 1);
}

static void notch_cb(Subject *subj, void *user_data) {
    (void)subj; (void)user_data;
    notch_apply();
}

/* ---- the scan ------------------------------------------------------------------------ */

static atomic_bool  scanning, stop_req;
static birdie_progress_fn progress_fn;
static void        *progress_arg;

typedef struct {
    char text[96];
    bool done;
} progress_t;

static void progress_post_cb(void *arg) {
    progress_t *p = arg;
    if (progress_fn) progress_fn(p->text, p->done, progress_arg);
    if (p->done) spot_marks_refresh();
}

static void progress(bool done, const char *fmt, ...) {
    progress_t p = {.done = done};
    va_list    ap;

    va_start(ap, fmt);
    vsnprintf(p.text, sizeof(p.text), fmt, ap);
    va_end(ap);
    scheduler_put(progress_post_cb, &p, sizeof(p));
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
    usleep(ms * 1000);
}

/* The 25th percentile of the dB values: the noise floor */
static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float floor_db(const float *db, uint16_t n) {
    static float sorted[WATERFALL_NFFT];
    memcpy(sorted, db, n * sizeof(float));
    qsort(sorted, n, sizeof(float), cmp_float);
    return sorted[n / 4];
}

/* The narrow peaks of a spectrum: their bins into out, returns how many */
static int peaks(const float *db, uint16_t n, uint16_t *out, int max) {
    float fl = floor_db(db, n);
    int   k  = 0;

    for (int i = EDGE_BINS; i < n - EDGE_BINS && k < max; i++) {
        if (abs(i - n / 2) <= CENTRE_BINS) continue;
        if (db[i] < fl + OVER_FLOOR_DB) continue;
        if (db[i] < db[i - 1] || db[i] < db[i + 1]) continue;
        float around = db[i - 4] > db[i + 4] ? db[i - 4] : db[i + 4];
        if (db[i] - around < NARROW_DB) continue;
        out[k++] = i;
    }
    return k;
}

/* A spectrum taken at f, where the scan just tuned the hardware: two fresh frames after the
 * settle (the frame's own centre follows the settings' VFO, which the scan leaves alone, so it
 * cannot be matched against f); false when the scan was stopped or no frame came */
static bool spectrum_at(int32_t f, float *db, uint32_t *span) {
    uint32_t center = 0, seq = 0, first = 0;
    uint64_t until = now_ms() + 1500;

    (void)f;
    if (!dsp_spectrum_snapshot(db, WATERFALL_NFFT, &center, span, &first)) first = 0;
    while (now_ms() < until) {
        if (atomic_load(&stop_req)) return false;
        if (dsp_spectrum_snapshot(db, WATERFALL_NFFT, &center, span, &seq) && seq >= first + 2)
            return true;
        sleep_ms(50);
    }
    return false;
}

static void *scan_thread(void *arg) {
    (void)arg;
    int32_t  start[64], stop[64];
    char     names[64][16];
    int      bands = cfg_bands_active(start, stop, names, 64);
    int32_t  dial  = cparam_i_get(cfg_fg_freq);
    uint32_t span  = 0;
    float    a[WATERFALL_NFFT], b[WATERFALL_NFFT];
    uint16_t pa[64], pb[64];
    int      found = 0;

    for (int bi = 0; bi < bands && !atomic_load(&stop_req); bi++) {
        int in_band = 0;

        if (cfg_band_use(start[bi], stop[bi]) != BAND_USE_TX) {
            continue;   /* receive only (the X6200's FM broadcast and airband, the WR12): no transmit there to clean
                         * up for, minutes more */
        }

        /* a first spectrum for the width in use */
        radio_set_freq(start[bi] + 50000 < stop[bi] ? start[bi] + 50000 : (start[bi] + stop[bi]) / 2);
        sleep_ms(SETTLE_MS);
        if (!spectrum_at(start[bi] + 50000 < stop[bi] ? start[bi] + 50000 : (start[bi] + stop[bi]) / 2, a, &span) || span < 10000)
            continue;
        int32_t step = (int32_t)span * 3 / 4;

        for (int32_t f = start[bi] + (int32_t)span / 2; f - (int32_t)span / 2 < stop[bi] && !atomic_load(&stop_req);
             f += step) {
            progress(false, "Scanning %s at %.3f MHz, %d found so far", names[bi], f / 1e6, found);
            radio_set_freq(f);
            sleep_ms(SETTLE_MS);
            if (!spectrum_at(f, a, &span)) continue;
            sleep_ms(AGAIN_MS);
            if (!spectrum_at(f, b, &span)) continue;

            int na = peaks(a, WATERFALL_NFFT, pa, 64), nb = peaks(b, WATERFALL_NFFT, pb, 64);
            for (int i = 0; i < na; i++) {
                bool steady = false;
                for (int j = 0; j < nb && !steady; j++) steady = abs((int)pa[i] - (int)pb[j]) <= 1;
                if (!steady) continue;
                int32_t hz = f - (int32_t)span / 2 + (int32_t)((int64_t)pa[i] * span / WATERFALL_NFFT);
                if (hz < start[bi] || hz > stop[bi]) continue;
                if (list_add(hz)) {
                    found++;
                    in_band++;
                }
            }
        }
        if (in_band) progress(false, "%s: %d new", names[bi], in_band);
    }
    radio_set_freq(dial);
    list_save();
    atomic_store(&scanning, false);
    if (atomic_load(&stop_req))
        progress(true, "Stopped: %d found, %d known in all, saved.", found, birdies_n);
    else
        progress(true, "Done: %d found, %d known in all, saved.", found, birdies_n);
    return NULL;
}

bool birdie_scan_start(birdie_progress_fn fn, void *arg) {
    pthread_t th;

    if (atomic_load(&scanning)) return false;
    progress_fn  = fn;
    progress_arg = arg;
    atomic_store(&stop_req, false);
    atomic_store(&scanning, true);
    if (pthread_create(&th, NULL, scan_thread, NULL) != 0) {
        atomic_store(&scanning, false);
        return false;
    }
    pthread_detach(th);
    return true;
}

void birdie_scan_stop(void) {
    atomic_store(&stop_req, true);
}

bool birdie_scanning(void) {
    return atomic_load(&scanning);
}

void birdie_init(void) {
    list_load();
    subject_subscribe_delayed((Subject *)cfg_fg_freq, notch_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_cur_mode, notch_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_cur_filter_low, notch_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_cur_filter_high, notch_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_birdie_marks, notch_cb, NULL);
}
