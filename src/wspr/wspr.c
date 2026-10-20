/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - WSPR receive and transmit helpers
 */

#include "wspr.h"

#include <complex.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <liquid/liquid.h>

#include "wsprd.h"
#include "wsprsim_utils.h"

struct wspr_bb {
    nco_crcf        nco;
    msresamp_crcf   rs;
    float           *i, *q;
    unsigned        n;
    pthread_mutex_t lock;
};

wspr_bb_t *wspr_bb_create(uint32_t audio_rate) {
    wspr_bb_t *bb = calloc(1, sizeof(*bb));

    bb->nco = nco_crcf_create(LIQUID_NCO);
    nco_crcf_set_frequency(bb->nco, 2.0f * M_PI * WSPR_AUDIO_HZ / audio_rate);
    /* Keeps +-150 Hz around WSPR_AUDIO_HZ, 60 dB down outside */
    bb->rs = msresamp_crcf_create((float)WSPR_RATE / audio_rate, 60.0f);
    bb->i  = malloc(sizeof(float) * WSPR_SAMPLES);
    bb->q  = malloc(sizeof(float) * WSPR_SAMPLES);
    pthread_mutex_init(&bb->lock, NULL);
    return bb;
}

void wspr_bb_destroy(wspr_bb_t *bb) {
    if (!bb) {
        return;
    }
    nco_crcf_destroy(bb->nco);
    msresamp_crcf_destroy(bb->rs);
    free(bb->i);
    free(bb->q);
    pthread_mutex_destroy(&bb->lock);
    free(bb);
}

void wspr_bb_reset(wspr_bb_t *bb) {
    pthread_mutex_lock(&bb->lock);
    msresamp_crcf_reset(bb->rs);
    bb->n = 0;
    pthread_mutex_unlock(&bb->lock);
}

void wspr_bb_feed(wspr_bb_t *bb, const float *samples, unsigned n) {
    float complex in[256], out[256];

    pthread_mutex_lock(&bb->lock);
    while (n && bb->n < WSPR_SAMPLES) {
        unsigned part = n < 256 ? n : 256;
        unsigned got;

        for (unsigned k = 0; k < part; k++) {
            nco_crcf_mix_down(bb->nco, samples[k], &in[k]);
            nco_crcf_step(bb->nco);
        }
        msresamp_crcf_execute(bb->rs, in, part, out, &got);
        for (unsigned k = 0; k < got && bb->n < WSPR_SAMPLES; k++, bb->n++) {
            bb->i[bb->n] = crealf(out[k]);
            bb->q[bb->n] = cimagf(out[k]);
        }
        samples += part;
        n -= part;
    }
    pthread_mutex_unlock(&bb->lock);
}

unsigned wspr_bb_take(wspr_bb_t *bb, float *i, float *q) {
    unsigned n;

    pthread_mutex_lock(&bb->lock);
    n = bb->n;
    memcpy(i, bb->i, sizeof(float) * n);
    memcpy(q, bb->q, sizeof(float) * n);
    pthread_mutex_unlock(&bb->lock);
    return n;
}

/* wsprd keeps its FFT plan in a global */
static pthread_mutex_t decode_lock = PTHREAD_MUTEX_INITIALIZER;

int wspr_decode_iq(float *i, float *q, unsigned n, uint32_t dial_hz, wspr_spot_t *out, int max) {
    static struct decoder_results res[MAX_UNIQUES];
    struct decoder_options        opt = {0};
    float                         peak = 1e-24f;
    int                           n_res = 0;

    /* Silence after the samples, then the level wsprd expects (-3 dB) */
    for (unsigned k = n; k < WSPR_SAMPLES; k++) {
        i[k] = q[k] = 0.0f;
    }
    for (unsigned k = 0; k < n; k++) {
        peak = fmaxf(peak, fmaxf(fabsf(i[k]), fabsf(q[k])));
    }
    for (unsigned k = 0; k < n; k++) {
        i[k] *= 0.5f / peak;
        q[k] *= 0.5f / peak;
    }

    opt.freq        = dial_hz;
    opt.npasses     = 2;
    opt.subtraction = 1;

    pthread_mutex_lock(&decode_lock);
    wspr_decode(i, q, WSPR_SAMPLES, opt, res, &n_res);
    if (n_res > max) {
        n_res = max;
    }
    for (int k = 0; k < n_res; k++) {
        wspr_spot_t *s = &out[k];

        s->snr     = lroundf(res[k].snr);
        s->dt      = res[k].dt;
        s->drift   = lroundf(res[k].drift);
        s->freq_hz = res[k].freq * 1e6;
        snprintf(s->call, sizeof(s->call), "%s", res[k].call);
        snprintf(s->loc, sizeof(s->loc), "%s", res[k].loc);
        s->dbm = atoi(res[k].pwr);
    }
    pthread_mutex_unlock(&decode_lock);
    return n_res;
}

bool wspr_encode(const char *call, const char *grid, int dbm, uint8_t symbols[WSPR_SYMBOLS]) {
    char   msg[32];
    size_t len = strlen(call);

    /* Type 1: plain call of 4..6 characters, digit in 2nd or 3rd place */
    if (len < 4 || len > 6 || !(isdigit((unsigned char)call[1]) || isdigit((unsigned char)call[2]))) {
        return false;
    }
    for (size_t k = 0; k < len; k++) {
        if (!isalnum((unsigned char)call[k])) {
            return false;
        }
    }
    if (strlen(grid) < 4 || !isalpha((unsigned char)grid[0]) || !isalpha((unsigned char)grid[1]) ||
        !isdigit((unsigned char)grid[2]) || !isdigit((unsigned char)grid[3]) ||
        toupper(grid[0]) > 'R' || toupper(grid[1]) > 'R') {
        return false;
    }
    snprintf(msg, sizeof(msg), "%s %c%c%c%c %d", call, toupper(grid[0]), toupper(grid[1]), grid[2], grid[3], dbm);
    for (char *p = msg; *p; p++) {
        *p = toupper(*p);
    }
    /* It unpacks the message again to check it, with the hash tables */
    char *hashtab = calloc(HASHTAB_SIZE, HASHTAB_ENTRY_LEN);
    char *loctab  = calloc(HASHTAB_SIZE, LOCTAB_ENTRY_LEN);
    bool  ok      = hashtab && loctab && get_wspr_channel_symbols(msg, hashtab, loctab, symbols) != 0;

    free(hashtab);
    free(loctab);
    return ok;
}

int16_t *wspr_synth(const uint8_t symbols[WSPR_SYMBOLS], float f0, uint32_t rate, uint32_t *n) {
    double   spsym = rate * WSPR_SYMBOL_S;
    uint32_t len   = (uint32_t)(spsym * WSPR_SYMBOLS);
    uint32_t ramp  = rate / 100;    /* 10 ms on and off */
    int16_t *out   = malloc(sizeof(int16_t) * len);
    double   phi   = 0.0;

    if (!out) {
        return NULL;
    }
    for (uint32_t k = 0; k < len; k++) {
        int    sym = symbols[(uint32_t)(k / spsym)];
        double f   = f0 + (sym - 1.5) * WSPR_TONE_HZ;
        float  a   = 32767.0f * 0.8f;

        if (k < ramp) {
            a *= (float)k / ramp;
        } else if (len - k < ramp) {
            a *= (float)(len - k) / ramp;
        }
        out[k] = (int16_t)(sin(phi) * a);
        phi = fmod(phi + 2.0 * M_PI * f / rate, 2.0 * M_PI);
    }
    *n = len;
    return out;
}

int wspr_dbm(float watts) {
    static const int levels[] = {0, 3, 7, 10, 13, 17, 20, 23, 27, 30, 33, 37, 40, 43, 47, 50, 53, 57, 60};
    float dbm  = 10.0f * log10f(fmaxf(watts, 0.001f) * 1000.0f);
    int   best = 0;

    for (size_t k = 1; k < sizeof(levels) / sizeof(levels[0]); k++) {
        if (fabsf(levels[k] - dbm) < fabsf(levels[best] - dbm)) {
            best = k;
        }
    }
    return levels[best];
}
