/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - JTTY transmitter
 *
 *  The waveform is WSJT-X 3.2.0-rc1 lib/jtty/gen_jttywave.f90 (WSJT
 *  Development Group, K1JT et al., GPL-3.0): Gaussian frequency pulses,
 *  BT 2.0, modulation index 1, the first and the last tone extended by a
 *  symbol, a raised-cosine ramp of an eighth of a symbol at both ends.
 *  Computed sample by sample at AUDIO_PLAY_RATE, where a symbol is not a
 *  whole number of samples (1411.2), around TX_PLAYER_AUDIO_HZ; tx_player
 *  shifts the VFO so the lowest tone lands on the chosen frequency.
 */

#include "jtty_tx.h"

#include "audio.h"
#include "jtty.h"
#include "tx_player.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#define AMPLITUDE   30000.0f
#define QUEUE       8
#define GT          3001            /* pulse table over -1.5..1.5 symbols */
#define FREQ_MIN    200
#define FREQ_MAX    2700

typedef struct {
    uint8_t tones[JTTY_NSYM * JTTY_MAX_FRAMES];
    int     nsym;
} msg_t;

static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       thread;
static volatile bool   tx_on = false, stop_req = false, abort_req = false;
static msg_t           queue[QUEUE];
static int             q_head = 0, q_cnt = 0;
static uint16_t        freq = 1500;

static float           pulse[GT];
static bool            pulse_ok = false;

/* The message on the air */
static msg_t  cur;
static double spsym;                /* samples per symbol */
static long   k, nsamp, nramp;
static double phase;
static bool   stopping;

static void pulse_init(void)
{
    const double c = M_PI * sqrt(2.0 / log(2.0)), b = 2.0;

    for (int i = 0; i < GT; i++) {
        double t = -1.5 + 3.0 * i / (GT - 1);
        pulse[i] = (float)(0.5 * (erf(c * b * (t + 0.5)) - erf(c * b * (t - 0.5))));
    }
    pulse_ok = true;
}

static float g(double tt)
{
    double x = (tt + 1.5) * (GT - 1) / 3.0;
    int    i = (int)floor(x);

    if (i < 0 || i >= GT - 1)
        return 0.0f;
    return pulse[i] + (float)(x - i) * (pulse[i + 1] - pulse[i]);
}

/* Next queued message on the air; false if none */
static bool next_msg(void)
{
    bool ok = false;

    pthread_mutex_lock(&mux);
    if (q_cnt > 0 && !stop_req) {
        cur = queue[q_head];
        q_head = (q_head + 1) % QUEUE;
        q_cnt--;
        ok = true;
    }
    pthread_mutex_unlock(&mux);
    if (ok) {
        k = 0;
        nsamp = lround(cur.nsym * spsym);
        nramp = lround(spsym / 8.0);
    }
    return ok;
}

static size_t fill(int16_t *buf, size_t cnt, void *ctx)
{
    size_t out = 0;

    (void)ctx;
    while (out < cnt) {
        if (stop_req && !stopping) {
            /* Finish the frame on the air, then end with the ramp */
            long fs = lround(JTTY_NSYM * spsym), end = (k / fs + 1) * fs;
            if (end < nsamp)
                nsamp = end;
            pthread_mutex_lock(&mux);
            q_cnt = 0;
            pthread_mutex_unlock(&mux);
            stopping = true;
        }
        if (k >= nsamp && !next_msg())
            break;

        double u = k / spsym, f = 0;
        int    j0 = (int)floor(u);
        for (int j = j0 - 1; j <= j0 + 1; j++) {
            int jj = j < 0 ? 0 : (j >= cur.nsym ? cur.nsym - 1 : j);
            f += cur.tones[jj] * g(u - j - 0.5);
        }
        f = TX_PLAYER_AUDIO_HZ + JTTY_BAUD * f;

        float env = 1.0f;
        if (k < nramp)
            env = (float)(1.0 - cos(M_PI * k / nramp)) / 2.0f;
        else if (k >= nsamp - nramp)
            env = (float)(1.0 + cos(M_PI * (k - (nsamp - nramp)) / nramp)) / 2.0f;

        buf[out++] = (int16_t)(AMPLITUDE * env * sinf((float)phase));
        phase = fmod(phase + 2.0 * M_PI * f / AUDIO_PLAY_RATE, 2.0 * M_PI);
        k++;
    }
    return out;
}

static bool abort_cb(void *ctx)
{
    (void)ctx;
    return abort_req;
}

static void *tx_thread_fn(void *arg)
{
    (void)arg;
    spsym = AUDIO_PLAY_RATE / (double)JTTY_BAUD;
    phase = 0;
    k = nsamp = 0;
    stopping = false;
    tx_player_stream(fill, NULL, freq, tx_player_base_gain_offset(), abort_cb, NULL);
    pthread_mutex_lock(&mux);
    q_cnt = 0;
    tx_on = false;
    stop_req = false;
    pthread_mutex_unlock(&mux);
    return NULL;
}

uint16_t jtty_tx_get_freq(void)
{
    return freq;
}

void jtty_tx_set_freq(uint16_t hz)
{
    if (hz < FREQ_MIN)
        hz = FREQ_MIN;
    if (hz > FREQ_MAX)
        hz = FREQ_MAX;
    freq = hz;
}

bool jtty_tx_send(const char *text, int profile, char *canon)
{
    uint64_t frames[JTTY_MAX_FRAMES];
    msg_t    m;
    int      nf = jtty_pack_text(text, profile, frames, canon);
    bool     start = false, ok = false;

    if (nf <= 0)
        return false;
    for (int i = 0; i < nf; i++)
        jtty_frame_tones(frames[i], m.tones + i * JTTY_NSYM);
    m.nsym = nf * JTTY_NSYM;

    if (!pulse_ok)
        pulse_init();
    pthread_mutex_lock(&mux);
    if (q_cnt < QUEUE && !stop_req) {
        queue[(q_head + q_cnt) % QUEUE] = m;
        q_cnt++;
        ok = true;
        if (!tx_on) {
            tx_on = true;
            abort_req = false;
            start = true;
        }
    }
    pthread_mutex_unlock(&mux);

    if (start) {
        if (pthread_create(&thread, NULL, tx_thread_fn, NULL) != 0) {
            pthread_mutex_lock(&mux);
            q_cnt = 0;
            tx_on = false;
            pthread_mutex_unlock(&mux);
            return false;
        }
        pthread_detach(thread);
    }
    return ok;
}

void jtty_tx_stop(void)
{
    if (tx_on)
        stop_req = true;
}

void jtty_tx_abort(void)
{
    if (tx_on)
        abort_req = true;
}

bool jtty_tx_is_on(void)
{
    return tx_on;
}

int jtty_tx_frames_left(void)
{
    int n;

    if (!tx_on)
        return 0;
    pthread_mutex_lock(&mux);
    /* Before the TX thread set its timing (tx_on is up first) nothing is on the air yet: the
     * queue alone counts, the division by zero would make NaN and INT_MIN */
    n = spsym > 0 ? (int)((nsamp - k) / (JTTY_NSYM * spsym) + 0.999) : 0;
    for (int i = 0; i < q_cnt; i++)
        n += queue[(q_head + i) % QUEUE].nsym / JTTY_NSYM;
    pthread_mutex_unlock(&mux);
    return n;
}
