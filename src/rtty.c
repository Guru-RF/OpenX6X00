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
#include "theme.h"
#include "tx_player.h"
#include "digi_textlog.h"
#include "digi_qso.h"           /* the DX call from "DE <call>", for the macros */
#include "rtty_tx.h"            /* the ITA2 table and the transmitter */

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
/* The LVGL thread opens and closes the app, the TX thread takes it from TX back to RX */
static _Atomic rtty_state_t state = RTTY_OFF;

static radio_mode_t cur_mode;

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

    return rx_letter ? rtty_ita2_letters[c] : rtty_ita2_figures[c];
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

/* The sideband for rtty_mark_high: +1 USB, -1 LSB, 0 another mode */
static int side(void) {
    switch (cur_mode) {
        case radio_mode_usb: case radio_mode_usb_dig: return 1;
        case radio_mode_lsb: case radio_mode_lsb_dig: return -1;
        default:                                      return 0;
    }
}

/* The text log's frequency: the middle of mark and space, the RTTY centre the dial's side of the audio */
static uint32_t log_freq(void) {
    uint32_t dial = (uint32_t)cparam_i_get(cfg_fg_freq);
    bool     lsb  = cur_mode == radio_mode_lsb || cur_mode == radio_mode_lsb_dig;

    return lsb ? dial - params.rtty_center : dial + params.rtty_center;
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
                        digi_textlog_rx("RTTY", log_freq(), DIGI_TEXTLOG_SIG_NONE, 0, str);   /* the card (#55) */
                        digi_qso_feed_rx(c);    /* "DE <call>": the DX call the macros send to (#312) */
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

    bool  mark_hi = rtty_mark_high(side(), params.rtty_reverse);
    float bw = params.rtty_rate / 100.0f * 1.5f;   /* a baud and a half */
    float mark_hz, space_hz;
    float c_mark[3], c_space[3];

    rtty_tones(params.rtty_center, params.rtty_shift, mark_hi, &mark_hz, &space_hz);
    bandpass_coef(c_mark, mark_hz, bw);
    bandpass_coef(c_space, space_hz, bw);

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

        if (mark_hi) {
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

/* A shortcut's rate, shift and reverse for RTTY's next opening (rtty_open_with), and the operator's own while RTTY
 * is open on them. LVGL thread, as rtty_set_state */
static struct {
    bool     set;
    uint16_t rate, shift;
    bool     reverse;
} next_settings, own_settings;

/* Rate, shift and reverse at once, the decoder made again for them. dirty: saved to the card as well (the
 * operator's own); a shortcut's are not, so a power cut while RTTY is open on them leaves the operator's there */
static void settings_set(uint16_t rate, uint16_t shift, bool reverse, bool dirty) {
    params_lock();
    params.rtty_rate = rate;
    params_unlock(dirty ? &params.dirty.rtty_rate : NULL);
    params_lock();
    params.rtty_shift = shift;
    params_unlock(dirty ? &params.dirty.rtty_shift : NULL);
    params_lock();
    params.rtty_reverse = reverse;
    params_unlock(dirty ? &params.dirty.rtty_reverse : NULL);
    update();
}

int rtty_preset_now(void) {
    return rtty_preset_find(params.rtty_rate, params.rtty_shift, params.rtty_reverse);
}

/* Saved as the knobs save them. RTTY opened by a shortcut (GhostNet): the operator's own come back at the close,
 * over a preset as over a knob */
void rtty_preset_set(int preset) {
    const rtty_preset_t *p;

    if (preset < 0 || preset >= RTTY_PRESETS) return;
    p = &rtty_presets[preset];
    settings_set(p->rate, p->shift, p->reverse, true);
}

void rtty_open_with(uint16_t rate, uint16_t shift, bool reverse) {
    next_settings.set     = true;
    next_settings.rate    = rate;
    next_settings.shift   = shift;
    next_settings.reverse = reverse;
}

void rtty_set_state(rtty_state_t x) {
    if (x == RTTY_OFF && state != RTTY_OFF) {
        digi_textlog_end();     /* the app closes: its last line onto the card */
        if (own_settings.set) {
            /* Opened by a shortcut: the operator's own rate, shift and reverse again, also after a knob turned them */
            own_settings.set = false;
            settings_set(own_settings.rate, own_settings.shift, own_settings.reverse, true);
        }
    } else if (x != RTTY_OFF && state == RTTY_OFF && next_settings.set) {
        /* Opened by a shortcut: its settings for this time, the operator's kept for the close */
        next_settings.set = false;
        if (!own_settings.set) {
            own_settings.set     = true;
            own_settings.rate    = params.rtty_rate;
            own_settings.shift   = params.rtty_shift;
            own_settings.reverse = params.rtty_reverse;
        }
        settings_set(next_settings.rate, next_settings.shift, next_settings.reverse, false);
    }
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

/* ---- TX (#312): the lines of the RTTY buttons (rtty_send.c), sent by rtty_tx.c through tx_player, as the other
 * digital apps send: the power cap, the ALC levelling, the SWR protection, the amateur bands only. A line queued
 * while one goes out is sent after it in the same transmission; Stop TX unkeys at the next block of audio and drops
 * what is left ---- */

#define TX_QUEUE_MAX    1024    /* characters waiting */

static pthread_mutex_t tx_mux = PTHREAD_MUTEX_INITIALIZER;
static char            tx_queue[TX_QUEUE_MAX + 1];  /* what is still to go, each line with its '\n' */
static atomic_bool     tx_on;           /* the TX thread runs: from the first line queued to the end */
static atomic_bool     tx_stop;         /* Stop TX */
static atomic_bool     tx_filled;       /* the player asked for audio: the transmission went ahead */
static uint32_t        tx_freq;         /* for the text log, as log_freq() at the start */
static char            tx_line[DIGI_TEXTLOG_TEXT_MAX + 1];  /* the line going out, for the text log */
static size_t          tx_line_len;

/* The next character of the queue, -1 when it is empty. TX thread */
static int tx_next(void *ctx) {
    int c = -1;

    (void)ctx;
    pthread_mutex_lock(&tx_mux);
    if (tx_queue[0]) {
        c = (unsigned char)tx_queue[0];
        memmove(tx_queue, tx_queue + 1, strlen(tx_queue));
    }
    pthread_mutex_unlock(&tx_mux);
    return c;
}

static void tx_log_line(void) {
    if (tx_line_len) {
        tx_line[tx_line_len] = '\0';
        digi_textlog_tx("RTTY", tx_freq, tx_line);
        tx_line_len = 0;
    }
}

/* A character on its way to the air: into the panel in the TX colour, its line into the text log at its end */
static void tx_echo(char c, void *ctx) {
    char s[2] = {c, '\0'};

    (void)ctx;
    panel_add_tx_text(s);
    if (c == '\n') {
        tx_log_line();
        return;
    }
    if (tx_line_len == DIGI_TEXTLOG_TEXT_MAX) tx_log_line();
    tx_line[tx_line_len++] = c;
}

static size_t tx_fill(int16_t *buf, size_t n, void *ctx) {
    atomic_store(&tx_filled, true);
    return rtty_tx_fill(buf, n, ctx);
}

static bool tx_abort_cb(void *ctx) {
    (void)ctx;
    return atomic_load(&tx_stop) || state != RTTY_TX;    /* Stop TX, or the app closed */
}

/* The decoder from where a received character starts, in letters, as at its opening: what it was in the middle of
 * when the radio keyed is gone */
static void rx_restart(void) {
    pthread_mutex_lock(&rtty_mux);
    rx_state  = RX_STATE_IDLE;
    rx_letter = true;
    pthread_mutex_unlock(&rtty_mux);
}

static void *tx_thread(void *arg) {
    rtty_tx_cfg_t *cfg = arg;
    bool           again;

    do {
        rtty_tx_t tx;
        bool      ok;

        rtty_tx_start(&tx, cfg, tx_next, tx_echo, NULL);
        atomic_store(&tx_filled, false);
        ok = tx_player_stream(tx_fill, &tx, TX_PLAYER_AUDIO_HZ, tx_player_base_gain_offset(), tx_abort_cb, NULL);
        if (tx_line_len) {
            panel_add_tx_text("\n");     /* stopped in a line: what comes next on a line of its own */
            tx_log_line();
        }
        if (!atomic_load(&tx_filled)) {
            /* tx_player sent nothing: another transmission (a TUNE, TCI, the PTT) holds the radio */
            msg_schedule_text_fmt("RTTY: not sent, the radio is transmitting");
        }
        /* A line queued while the tail went out goes in a transmission of its own. Else what is left goes: after Stop
         * TX, a refusal (out of the bands), the SWR protection or the app closed */
        pthread_mutex_lock(&tx_mux);
        again = ok && !atomic_load(&tx_stop) && tx_queue[0] && state == RTTY_TX;
        if (!again) {
            rtty_state_t was = RTTY_TX;

            tx_queue[0] = '\0';
            rx_restart();
            atomic_compare_exchange_strong(&state, &was, RTTY_RX);     /* not when the app closed meanwhile */
            atomic_store(&tx_on, false);
        }
        pthread_mutex_unlock(&tx_mux);
    } while (again);
    free(cfg);
    return NULL;
}

bool rtty_tx_send(const char *text) {
    rtty_tx_cfg_t *cfg;
    pthread_t      th;
    size_t         len;

    if (!text || !*text || state == RTTY_OFF) {
        return false;
    }
    /* The tones are for the sidebands: in AM or FM they would go out as another signal */
    if (side() == 0) {
        msg_update_text_fmt("#%06x RTTY TX: LSB or USB only#", theme_hex(TC_err));
        return false;
    }
    pthread_mutex_lock(&tx_mux);
    len = strlen(tx_queue);
    if (atomic_load(&tx_on) && state != RTTY_TX) {
        /* the last transmission still ending after the app closed and opened again */
        pthread_mutex_unlock(&tx_mux);
        msg_update_text_fmt("#%06x RTTY: TX busy#", theme_hex(TC_err));
        return false;
    }
    if (len + strlen(text) + 1 > TX_QUEUE_MAX) {
        pthread_mutex_unlock(&tx_mux);
        msg_update_text_fmt("#%06x RTTY: TX queue full#", theme_hex(TC_err));
        return false;
    }
    snprintf(tx_queue + len, sizeof(tx_queue) - len, "%s\n", text);
    if (atomic_load(&tx_on)) {
        pthread_mutex_unlock(&tx_mux);
        return true;    /* after what is going out */
    }

    /* A new transmission, on the app's settings as they are now: rate, shift, centre and the mark the receiver takes */
    cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        tx_queue[0] = '\0';
        pthread_mutex_unlock(&tx_mux);
        return false;
    }
    cfg->rate = AUDIO_PLAY_RATE;
    cfg->baud = params.rtty_rate / 100.0f;
    rtty_tones(params.rtty_center, params.rtty_shift, rtty_mark_high(side(), params.rtty_reverse), &cfg->mark_hz,
               &cfg->space_hz);
    tx_freq = log_freq();
    tx_line_len = 0;
    atomic_store(&tx_stop, false);
    atomic_store(&tx_on, true);
    state = RTTY_TX;    /* no RX feed while sending (dsp.cpp) */
    if (pthread_create(&th, NULL, tx_thread, cfg)) {
        state = RTTY_RX;
        atomic_store(&tx_on, false);
        tx_queue[0] = '\0';
        pthread_mutex_unlock(&tx_mux);
        free(cfg);
        return false;
    }
    pthread_detach(th);
    pthread_mutex_unlock(&tx_mux);
    return true;
}

void rtty_tx_stop(void) {
    pthread_mutex_lock(&tx_mux);
    if (atomic_load(&tx_on)) {
        atomic_store(&tx_stop, true);
        tx_queue[0] = '\0';
        msg_update_text_fmt("RTTY TX stopped");
    }
    pthread_mutex_unlock(&tx_mux);
}

bool rtty_tx_on(void) {
    return atomic_load(&tx_on);
}
