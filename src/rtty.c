/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */
#include "rtty.h"

#include "dsp.h"
#include "audio.h"
#include "panel.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "util.h"
#include "msg.h"
#include "tx_player.h"

#include "lvgl/lvgl.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define SYMBOL_OVER 8
#define SYMBOL_FACTOR 2
#define SYMBOL_LEN (SYMBOL_OVER * SYMBOL_FACTOR)

#define RTTY_SYMBOL_CODE (0b11011)
#define RTTY_LETTER_CODE (0b11111)

#define CAPTURE_RATE_F ((float)AUDIO_CAPTURE_RATE / AUDIO_DECIM)

typedef enum {
    RX_STATE_IDLE,
    RX_STATE_START,
    RX_STATE_DATA,
    RX_STATE_STOP
} rx_state_t;

static pthread_mutex_t rtty_mux;

static fskdem demod = NULL;

static nco_crcf       nco     = NULL;
static float complex *nco_buf = NULL;

static uint16_t symbol_samples;
static uint16_t symbol_over;

static cbuffercf      rx_buf;
static firhilbf       hilb;
static complex float *rx_window = NULL;
static uint8_t        rx_symbol[SYMBOL_LEN];
static float          rx_symbol_pwr[SYMBOL_LEN];
static uint8_t        rx_symbol_cur = 0;
static rx_state_t     rx_state      = RX_STATE_IDLE;
static uint8_t        rx_counter    = 0;
static uint8_t        rx_bitcntr    = 0;
static uint8_t        rx_data       = 0;
static bool           rx_letter     = true;

static bool         ready = false;
static rtty_state_t state = RTTY_OFF;

static x6100_mode_t cur_mode;

static const char rtty_letters[32] = {'\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\0', 'D', 'R',
                                      'J',  'N', 'F',  'C', 'K', 'T', 'Z', 'L', 'W',  'H', 'Y',
                                      'P',  'Q', 'O',  'B', 'G', ' ', 'M', 'X', 'V',  ' '};

static const char rtty_symbols[32] = {'\0', '3', '\n', '-', ' ', '\0', '8', '7', '\0', '$', '4',
                                      '\'', ',', '!',  ':', '(', '5',  '"', ')', '2',  '#', '6',
                                      '0',  '1', '9',  '?', '&', ' ',  '.', '/', ';',  ' '};

static void on_cur_mode_change(Subject *subj, void *user_data);

/* Tuning scope: the audio through a mark and a space band-pass, X and Y of the old
 * crossed-ellipse display; and where the two tones really are, from the phase steps
 * of the decided symbols (offset of their middle from the centre, Hz) */
static float    scope_x[RTTY_SCOPE_LEN];
static float    scope_y[RTTY_SCOPE_LEN];
static uint16_t scope_pos;
static float    bp_mark[4], bp_space[4];    /* x1 x2 y1 y2 */
static float    tone_hi, tone_lo;
static float    tone_var = 1e6f;            /* spread of the symbols around their tone, Hz^2 */
static uint32_t tone_ms;                    /* last decided symbol */

/* RBJ band-pass, 0 dB at f, bw Hz wide: b0, a1, a2 (b1 = 0, b2 = -b0) */
static void bandpass_coef(float *c, float f, float bw) {
    float w0 = 2.0f * M_PI * f / CAPTURE_RATE_F;
    float alpha = sinf(w0) * bw / (2.0f * f);
    float a0 = 1.0f + alpha;

    c[0] = alpha / a0;
    c[1] = -2.0f * cosf(w0) / a0;
    c[2] = (1.0f - alpha) / a0;
}

static float bandpass(float *st, const float *c, float x) {
    float y = c[0] * (x - st[1]) - c[1] * st[2] - c[2] * st[3];

    st[1] = st[0]; st[0] = x;
    st[3] = st[2]; st[2] = y;
    return y;
}

static void update_nco() {
    float radians = 2.0f * M_PI * params.rtty_center / CAPTURE_RATE_F;

    nco_crcf_set_phase(nco, 0.0f);
    nco_crcf_set_frequency(nco, radians);
}

static void init() {
    symbol_samples = CAPTURE_RATE_F / ((float)params.rtty_rate / 100.0f) / (float)SYMBOL_FACTOR + 0.5f;
    symbol_over    = symbol_samples / SYMBOL_OVER;

    hilb = firhilbf_create(41, 60.0f);

    nco     = nco_crcf_create(LIQUID_NCO);
    nco_buf = (float complex *)malloc(symbol_samples * sizeof(float complex));
    update_nco();

    /* RX */

    demod  = fskdem_create(1, symbol_samples, (float)params.rtty_shift / CAPTURE_RATE_F / 2.0f);
    rx_buf = cbuffercf_create(symbol_samples * 50);

    rx_window = malloc(symbol_samples * sizeof(complex float));

    for (uint16_t i = 0; i < symbol_samples; i++)
        rx_window[i] = liquid_hann(i, symbol_samples);

    tone_hi = params.rtty_shift / 2.0f;
    tone_lo = -tone_hi;
    tone_var = 1e6f;
    ready = true;
}

static void done() {
    ready = false;

    if (nco) {
        nco_crcf_destroy(nco);
        nco = NULL;
    }
    if (nco_buf) {
        free(nco_buf);
        nco_buf = NULL;
    }

    if (hilb) {
        firhilbf_destroy(hilb);
        hilb = NULL;
    }

    if (demod) {
        fskdem_destroy(demod);
        demod = NULL;
    }

    if (rx_buf) {
        cbuffercf_destroy(rx_buf);
        rx_buf = NULL;
    }
    if (rx_window) {
        free(rx_window);
        rx_window = NULL;
    }
}

static void update() {
    pthread_mutex_lock(&rtty_mux);
    done();
    init();
    pthread_mutex_unlock(&rtty_mux);
}

void rtty_init() {
    pthread_mutex_init(&rtty_mux, NULL);
    subject_subscribe_and_notify((Subject*)cfg_cur_mode, on_cur_mode_change, NULL);
    init();
}

static char baudot_decoder(uint8_t c) {
    if (c == RTTY_SYMBOL_CODE) {
        rx_letter = false;
        return 0;
    }

    if (c == RTTY_LETTER_CODE) {
        rx_letter = true;
        return 0;
    }

    return rx_letter ? rtty_letters[c] : rtty_symbols[c];
}

static bool is_mark_space(uint8_t *correction) {
    uint16_t res = 0;

    if (rx_symbol[0] && !rx_symbol[SYMBOL_LEN - 1]) {
        for (int i = 0; i < SYMBOL_LEN; i++)
            res += rx_symbol[i];

        if (abs(SYMBOL_LEN / 2 - res) < 1) {
            *correction = res;
            return true;
        }
    }
    return false;
}

static bool is_mark() {
    return rx_symbol[SYMBOL_LEN / 2];
}

static void add_symbol(float pwr) {
    for (uint8_t i = 1; i < SYMBOL_LEN; i++) {
        rx_symbol[i - 1]     = rx_symbol[i];
        rx_symbol_pwr[i - 1] = rx_symbol_pwr[i];
    }

    rx_symbol_pwr[SYMBOL_LEN - 1] = pwr;

    float   p_avr = 0.0f;
    uint8_t p_num = SYMBOL_LEN / 2;

    for (uint8_t i = SYMBOL_LEN - p_num; i < SYMBOL_LEN; i++)
        p_avr += rx_symbol_pwr[i];

    p_avr /= (float)p_num;

    if (rx_symbol_cur == 0) {
        if (p_avr > params.rtty_snr) {
            rx_symbol_cur = 1;
        }
    } else {
        if (p_avr < -params.rtty_snr) {
            rx_symbol_cur = 0;
        }
    }

    /* LV_LOG_USER("%5.1f %i", p_avr, rx_symbol_cur); */

    rx_symbol[SYMBOL_LEN - 1] = rx_symbol_cur;

    uint8_t correction;

    switch (rx_state) {
        case RX_STATE_IDLE:
            if (is_mark_space(&correction)) {
                rx_state   = RX_STATE_START;
                rx_counter = correction;
            }
            break;

        case RX_STATE_START:
            if (--rx_counter == 0) {
                if (!is_mark()) {
                    rx_state   = RX_STATE_DATA;
                    rx_counter = SYMBOL_LEN;
                    rx_bitcntr = 0;
                    rx_data    = 0;
                } else {
                    rx_state = RX_STATE_IDLE;
                }
            }
            break;

        case RX_STATE_DATA:
            if (--rx_counter == 0) {
                rx_data |= is_mark() << rx_bitcntr++;
                rx_counter = SYMBOL_LEN;
            }

            if (rx_bitcntr == params.rtty_bits)
                rx_state = RX_STATE_STOP;
            break;

        case RX_STATE_STOP:
            if (--rx_counter == 0) {
                if (is_mark()) {
                    char c = baudot_decoder(rx_data);

                    if (c) {
                        char str[2] = {c, 0};

                        panel_add_text(str);
                    }
                }
                rx_state = RX_STATE_IDLE;
            }
            break;
    }
}

void rtty_put_audio_samples(unsigned int n, float *samples) {
    pthread_mutex_lock(&rtty_mux);

    if (!ready) {
        pthread_mutex_unlock(&rtty_mux);
        return;
    }

    bool  mark_hi = ((cur_mode == x6100_mode_usb || cur_mode == x6100_mode_usb_dig) && !params.rtty_reverse) ||
                    ((cur_mode == x6100_mode_lsb || cur_mode == x6100_mode_lsb_dig) && params.rtty_reverse);
    float half = params.rtty_shift / 2.0f;
    float bw = params.rtty_rate / 100.0f * 1.5f;   /* a baud and a half */
    float c_mark[3], c_space[3];

    bandpass_coef(c_mark, params.rtty_center + (mark_hi ? half : -half), bw);
    bandpass_coef(c_space, params.rtty_center + (mark_hi ? -half : half), bw);

    for (size_t i = 0; i < n; i++) {
        cfloat cval;
        firhilbf_r2c_execute(hilb, samples[i], &cval);
        cbuffercf_push(rx_buf, cval);

        scope_x[scope_pos] = bandpass(bp_mark, c_mark, samples[i]);
        scope_y[scope_pos] = bandpass(bp_space, c_space, samples[i]);
        scope_pos = (scope_pos + 1) % RTTY_SCOPE_LEN;
    }

    while (cbuffercf_size(rx_buf) > symbol_samples) {
        unsigned int   symbol;
        unsigned int   n;
        cfloat *buf;

        cbuffercf_read(rx_buf, symbol_samples, &buf, &n);
        nco_crcf_mix_block_down(nco, buf, nco_buf, n);

        float complex step = 0.0f;
        for (uint16_t i = 1; i < n; i++)
            step += nco_buf[i] * conjf(nco_buf[i - 1]);

        for (uint16_t i = 0; i < n; i++)
            nco_buf[i] *= rx_window[i];

        fskdem_demodulate(demod, nco_buf);

        float pwr0 = 10.0f * log10f(fskdem_get_symbol_energy(demod, 0, 1));
        float pwr1 = 10.0f * log10f(fskdem_get_symbol_energy(demod, 1, 1));
        float pwr  = pwr0 - pwr1;

        if (((cur_mode == x6100_mode_usb || cur_mode == x6100_mode_usb_dig) && !params.rtty_reverse) ||
            ((cur_mode == x6100_mode_lsb || cur_mode == x6100_mode_lsb_dig) && params.rtty_reverse)) {
            pwr = -pwr;
        }

        add_symbol(pwr);

        if (fabsf(pwr) > params.rtty_snr) {
            float hz = cargf(step) * CAPTURE_RATE_F / (2.0f * M_PI);
            float *tone = hz > 0.0f ? &tone_hi : &tone_lo;

            tone_var += ((hz - *tone) * (hz - *tone) - tone_var) * 0.05f;

            *tone += (hz - *tone) * 0.05f;
            tone_ms = lv_tick_get();
        }

        cbuffercf_release(rx_buf, symbol_over);
    }

    pthread_mutex_unlock(&rtty_mux);
}

bool rtty_scope_get(float *x, float *y, float *offset) {
    pthread_mutex_lock(&rtty_mux);
    for (uint16_t i = 0; i < RTTY_SCOPE_LEN; i++) {
        x[i] = scope_x[(scope_pos + i) % RTTY_SCOPE_LEN];
        y[i] = scope_y[(scope_pos + i) % RTTY_SCOPE_LEN];
    }
    *offset = (tone_hi + tone_lo) / 2.0f;
    /* Noise decides symbols too, but its "tones" wander: a real signal keeps each symbol
     * within a few tens of Hz of its tone, and the two tones a shift apart */
    bool tuned = lv_tick_elaps(tone_ms) < 2000 && tone_var < 30.0f * 30.0f &&
                 fabsf(tone_hi - tone_lo - params.rtty_shift) < params.rtty_shift * 0.25f;
    pthread_mutex_unlock(&rtty_mux);
    return tuned;
}

void rtty_set_state(rtty_state_t x) {
    state = x;
}

rtty_state_t rtty_get_state() {
    return state;
}

float rtty_change_rate(int16_t df) {
    if (df == 0) {
        return (float)params.rtty_rate / 100.0f;
    }

    params_lock();

    switch (params.rtty_rate) {
        case 4500:
            params.rtty_rate = df > 0 ? 4545 : 15000;
            break;

        case 4545:
            params.rtty_rate = df > 0 ? 5000 : 4500;
            break;

        case 5000:
            params.rtty_rate = df > 0 ? 5600 : 4545;
            break;

        case 5600:
            params.rtty_rate = df > 0 ? 7500 : 5000;
            break;

        case 7500:
            params.rtty_rate = df > 0 ? 10000 : 5600;
            break;

        case 10000:
            params.rtty_rate = df > 0 ? 11000 : 7500;
            break;

        case 11000:
            params.rtty_rate = df > 0 ? 15000 : 10000;
            break;

        case 15000:
            params.rtty_rate = df > 0 ? 4500 : 11000;
            break;

        default:
            params.rtty_rate = 4500;
            break;
    }

    params_unlock(&params.dirty.rtty_rate);
    update();

    return (float)params.rtty_rate / 100.0f;
}

uint16_t rtty_change_shift(int16_t df) {
    if (df == 0) {
        return params.rtty_shift;
    }

    params_lock();

    switch (params.rtty_shift) {
        case 170:
            params.rtty_shift = df > 0 ? 425 : 850;
            break;

        case 425:
            params.rtty_shift = df > 0 ? 450 : 170;
            break;

        case 450:
            params.rtty_shift = df > 0 ? 850 : 425;
            break;

        case 850:
            params.rtty_shift = df > 0 ? 170 : 450;
            break;

        default:
            params.rtty_shift = 170;
            break;
    }

    params_unlock(&params.dirty.rtty_shift);
    update();

    return params.rtty_shift;
}

uint16_t rtty_change_center(int16_t df) {
    if (df == 0) {
        return params.rtty_center;
    }

    params_lock();
    params.rtty_center = limit(align_int(params.rtty_center + df * 10, 10), 800, 1600);
    params_unlock(&params.dirty.rtty_center);

    pthread_mutex_lock(&rtty_mux);
    update_nco();
    pthread_mutex_unlock(&rtty_mux);

    return params.rtty_center;
}

bool rtty_change_reverse(int16_t df) {
    if (df == 0) {
        return params.rtty_reverse;
    }

    params_lock();
    params.rtty_reverse = !params.rtty_reverse;
    params_unlock(&params.dirty.rtty_reverse);

    return params.rtty_reverse;
}

static void on_cur_mode_change(Subject *subj, void *user_data) {
    cur_mode = cparam_i_get(cfg_cur_mode);
}

/* TX (web remote): the text in AFSK, ITA2 with 1 start, 5 data (LSB first) and 1.5 stop bits */

#define TX_TEXT_MAX 200
#define TX_AMP      (32767.0f * 0.8f)
#define TX_IDLE_S   1.0f    /* mark before the first character */
#define TX_TAIL     3.0f    /* bits of mark after the last one */

typedef struct {
    char  text[TX_TEXT_MAX + 1];
    float baud, mark_hz, space_hz;
    int   n;                          /* codes */
    uint8_t  codes[2 * TX_TEXT_MAX + 1];
    uint8_t  done[2 * TX_TEXT_MAX + 1];   /* characters of text on air once this code is */
    uint32_t end[2 * TX_TEXT_MAX + 1];    /* sample where this code ends */
    struct timespec start;
} tx_job_t;

typedef struct {
    int16_t *buf;
    uint32_t n;
    double   pos;   /* samples, fractional */
    float    phase;
} synth_t;

static atomic_bool tx_abort;
static atomic_int  tx_sent;

/* ITA2 code of c, -1 if none. *shift: the case it needs, 0 for both */
static int baudot_code(char c, uint8_t *shift) {
    for (int i = 0; c && i < 32; i++) {
        if (i == RTTY_SYMBOL_CODE || i == RTTY_LETTER_CODE)
            continue;
        bool l = rtty_letters[i] == c, f = rtty_symbols[i] == c;
        if (l || f) {
            *shift = (l && f) ? 0 : l ? RTTY_LETTER_CODE : RTTY_SYMBOL_CODE;
            return i;
        }
    }
    return -1;
}

/* LTRS first, then the characters with the case shifts; the ones Baudot lacks are dropped */
static void tx_encode(tx_job_t *job) {
    uint8_t shift = RTTY_LETTER_CODE;

    job->n        = 0;
    job->codes[0] = RTTY_LETTER_CODE;
    job->done[0]  = 0;
    job->n++;
    for (int i = 0; job->text[i]; i++) {
        uint8_t sh;
        int     code = baudot_code(toupper((unsigned char)job->text[i]), &sh);

        if (code < 0)
            continue;
        if (sh && sh != shift) {
            shift               = sh;
            job->codes[job->n]  = sh;
            job->done[job->n++] = i;
        }
        job->codes[job->n]  = code;
        job->done[job->n++] = i + 1;
    }
}

/* Continuous phase; bit edges on the fractional sample position */
static void tx_bits(synth_t *s, float hz, float bits, float spb) {
    float    dphi = 2.0f * (float)M_PI * hz / AUDIO_PLAY_RATE;
    uint32_t to;

    s->pos += bits * spb;
    to = (uint32_t)lround(s->pos);
    for (; s->n < to; s->n++) {
        s->phase += dphi;
        if (s->phase > 2.0f * (float)M_PI)
            s->phase -= 2.0f * (float)M_PI;
        s->buf[s->n] = (int16_t)(sinf(s->phase) * TX_AMP);
    }
}

static bool tx_abort_cb(void *ctx) {
    tx_job_t       *job = ctx;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!job->start.tv_sec)
        job->start = now;
    double played = ((now.tv_sec - job->start.tv_sec) + (now.tv_nsec - job->start.tv_nsec) * 1e-9) * AUDIO_PLAY_RATE;
    for (int i = 0; i < job->n && job->end[i] <= played; i++)
        atomic_store(&tx_sent, job->done[i]);
    return atomic_load(&tx_abort) || state != RTTY_TX;    /* stopped, or the app closed */
}

static void *tx_thread(void *arg) {
    tx_job_t *job    = arg;
    float     spb    = AUDIO_PLAY_RATE / job->baud;
    int       result = -2;

    tx_encode(job);

    /* ponytail: the whole waveform in memory (~6 MB for 200 characters at 45 baud); stream it when that hurts */
    synth_t  s   = {0};
    uint32_t cap = (uint32_t)((TX_IDLE_S * AUDIO_PLAY_RATE) + (job->n * 7.5f + TX_TAIL) * spb) + 16;

    s.buf = malloc(cap * sizeof(int16_t));
    if (s.buf) {
        tx_bits(&s, job->mark_hz, TX_IDLE_S * job->baud, spb);
        for (int i = 0; i < job->n; i++) {
            tx_bits(&s, job->space_hz, 1.0f, spb);
            for (int b = 0; b < 5; b++)
                tx_bits(&s, (job->codes[i] >> b) & 1 ? job->mark_hz : job->space_hz, 1.0f, spb);
            tx_bits(&s, job->mark_hz, 1.5f, spb);
            job->end[i] = s.n;
        }
        tx_bits(&s, job->mark_hz, TX_TAIL, spb);

        bool ok = tx_player_play(s.buf, s.n, TX_PLAYER_AUDIO_HZ, tx_player_base_gain_offset(), tx_abort_cb, job);
        result  = ok ? 1 : -1;
        if (ok)
            atomic_store(&tx_sent, (int)strlen(job->text));
        free(s.buf);
    }
    if (state == RTTY_TX)
        state = RTTY_RX;
    free(job);
    return NULL;
}

/* LVGL thread */
static void tx_start(const char *text) {
    pthread_t th;
    tx_job_t *job;

    if (state != RTTY_RX) {
        msg_update_text_fmt("RTTY TX busy");
        return;
    }
    job = calloc(1, sizeof(*job));
    if (!job)
        return;
    snprintf(job->text, sizeof(job->text), "%s", text);
    job->baud = params.rtty_rate / 100.0f;

    /* The tones the receiver takes for mark and space (rtty_put_audio_samples) */
    bool mark_hi = ((cur_mode == x6100_mode_usb || cur_mode == x6100_mode_usb_dig) && !params.rtty_reverse) ||
                   ((cur_mode == x6100_mode_lsb || cur_mode == x6100_mode_lsb_dig) && params.rtty_reverse);
    float half = params.rtty_shift / 2.0f;
    job->mark_hz  = params.rtty_center + (mark_hi ? half : -half);
    job->space_hz = params.rtty_center + (mark_hi ? -half : half);

    atomic_store(&tx_abort, false);
    atomic_store(&tx_sent, 0);
    state = RTTY_TX;    /* no RX feed while sending */
    if (pthread_create(&th, NULL, tx_thread, job)) {
        state = RTTY_RX;
        free(job);
        return;
    }
    pthread_detach(th);
}
