/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Olivia MFSK transmitter
 *
 *  From SP2KMO's OpenX6100 psk31/olivia work (randal007 merge). Here:
 *  the blocks are handed to tx_player_stream() (the TX path of the other
 *  digital apps: power cap, ALC levelling, SWR protection, the TX strip),
 *  modulated around the chosen centre with the VFO left where it is; 8/250
 *  is the default variant.
 */

#include "olivia_tx.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "tx_player.h"

#include "olivia.h"
#include "olivia_mod.h"
#include "olivia_resamp.h"

#define AMPLITUDE        30000.0f

/* The rate the modulation is defined at. */
#define OLIVIA_NOMINAL_RATE  8000

/* Blocks of zeros before the data. The receiver needs a few blocks to
 * find the synchronisation, so a run-in padded with zeros goes first -
 * fldigi does the same. */
#define LEAD_BLOCKS      4
#define TAIL_BLOCKS      1

#define CENTER_MIN       500
#define CENTER_MAX       2500
#define CENTER_DEFAULT   1500

static const struct {
    int tones;
    int bandwidth;
    const char *name;
} mode_table[OLIVIA_MODE_COUNT] = {
    {  8,  250, "8/250"   },
    { 16,  500, "16/500"  },
    { 32, 1000, "32/1000" },
};

static pthread_t     tx_thread;
static volatile bool tx_on = false;
static volatile bool tx_stop_req = false;
static volatile bool tx_abort_req = false;
static char         *tx_text = NULL;
static uint16_t      center_hz = CENTER_DEFAULT;
static olivia_mode_t cur_mode = OLIVIA_MODE_8_250;
static olivia_tx_echo_cb_t echo_cb = NULL;

/* --- Mode parameters -------------------------------------------------- */

olivia_mode_t olivia_tx_get_mode(void) {
    return cur_mode;
}

void olivia_tx_set_mode(olivia_mode_t mode) {
    if (mode < 0 || mode >= OLIVIA_MODE_COUNT) return;
    if (tx_on) return;                  /* not while transmitting */
    cur_mode = mode;
}

const char *olivia_tx_mode_name(olivia_mode_t mode) {
    if (mode < 0 || mode >= OLIVIA_MODE_COUNT) return "?";
    return mode_table[mode].name;
}

int olivia_tx_mode_tones(olivia_mode_t mode) {
    if (mode < 0 || mode >= OLIVIA_MODE_COUNT) return 8;
    return mode_table[mode].tones;
}

int olivia_tx_mode_bandwidth(olivia_mode_t mode) {
    if (mode < 0 || mode >= OLIVIA_MODE_COUNT) return 250;
    return mode_table[mode].bandwidth;
}

uint16_t olivia_tx_get_center(void) {
    return center_hz;
}

void olivia_tx_set_center(uint16_t hz) {
    if (hz < CENTER_MIN) hz = CENTER_MIN;
    if (hz > CENTER_MAX) hz = CENTER_MAX;
    center_hz = hz;
}

void olivia_tx_set_echo_cb(olivia_tx_echo_cb_t cb) {
    echo_cb = cb;
}

/* --- Modulator, block by block for tx_player_stream() ----------------- */

typedef struct {
    olivia_mod_t    mod;
    olivia_resamp_t res;
    float          *blk, *out;
    int             blk_samples;
    int             out_n, out_pos;     /* resampled samples ready */
    size_t          text_len, text_pos;
    int             lead, tail;
    bool            done;
} mod_t;

/* The next block of characters into m->out; false when all is sent. */
static bool next_block(mod_t *m) {
    uint8_t chars[OLIVIA_MAX_BITS];
    int     bits = m->mod.cfg.bits;

    if (m->done) return false;
    if (m->lead > 0 && !tx_stop_req) {
        memset(chars, 0, sizeof(chars));            /* run-in: zeros only */
        m->lead--;
    } else if (m->text_pos < m->text_len && !tx_stop_req) {
        for (int i = 0; i < bits; i++) {
            if (m->text_pos < m->text_len) {
                unsigned char c = (unsigned char)tx_text[m->text_pos++];

                if (c == '\n') c = '\r';    /* Olivia uses CR */
                if (c > 127)   c = '?';     /* the alphabet is 7-bit */
                chars[i] = c;
            } else {
                chars[i] = 0;               /* padding with zeros */
            }
        }
    } else if (m->tail > 0) {
        memset(chars, 0, sizeof(chars));
        if (--m->tail == 0) m->done = true;
    } else {
        return false;
    }
    olivia_mod_block(&m->mod, chars, m->blk);
    m->out_n   = olivia_resamp_process(&m->res, m->blk, m->blk_samples, m->out);
    m->out_pos = 0;
    return true;
}

static size_t mod_fill(int16_t *buf, size_t cnt, void *ctx) {
    mod_t *m = ctx;
    size_t n = 0;

    while (n < cnt) {
        if (m->out_pos >= m->out_n && !next_block(m)) break;
        while (n < cnt && m->out_pos < m->out_n) {
            float v = m->out[m->out_pos++] * AMPLITUDE;

            buf[n++] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
        }
    }
    return n;
}

/* --- Worker thread ---------------------------------------------------- */

static bool abort_cb(void *ctx) {
    (void)ctx;
    return tx_abort_req;
}

static void *tx_thread_fn(void *arg) {
    (void)arg;
    mod_t m = {.lead = LEAD_BLOCKS, .tail = TAIL_BLOCKS, .text_len = strlen(tx_text)};
    int   bw = mode_table[cur_mode].bandwidth;

    /* Around the centre chosen on the waterfall, the VFO left where it is (the offset
     * TX_PLAYER_AUDIO_HZ asks tx_player for no shift) */
    if (olivia_mod_init(&m.mod, mode_table[cur_mode].tones, bw, OLIVIA_NOMINAL_RATE,
                        center_hz - bw / 2.0) == 0) {
        if (olivia_resamp_init(&m.res, OLIVIA_NOMINAL_RATE, AUDIO_PLAY_RATE) == 0) {
            m.blk_samples = olivia_mod_block_samples(&m.mod);
            m.blk = malloc(sizeof(float) * m.blk_samples);
            m.out = malloc(sizeof(float) * olivia_resamp_max_out(&m.res, m.blk_samples));
            if (m.blk && m.out) {
                /* A different seed for every transmission, so that a
                 * repeated macro does not give an identical spectrum. */
                olivia_mod_seed(&m.mod, (uint32_t)time(NULL) | 1u);
                tx_player_stream(mod_fill, &m, TX_PLAYER_AUDIO_HZ, tx_player_base_gain_offset(), abort_cb, NULL);
            }
            free(m.blk);
            free(m.out);
            olivia_resamp_free(&m.res);
        }
        olivia_mod_free(&m.mod);
    }
    free(tx_text);
    tx_text     = NULL;
    tx_stop_req = false;
    tx_on       = false;
    return NULL;
}

/* --- Public API ------------------------------------------------------- */

bool olivia_tx_send(const char *text) {
    if (tx_on || !text || !*text) return false;

    tx_text = strdup(text);
    if (!tx_text) return false;

    tx_on = true;
    tx_stop_req = false;
    tx_abort_req = false;

    if (echo_cb) {
        echo_cb(text);
    }

    if (pthread_create(&tx_thread, NULL, tx_thread_fn, NULL) != 0) {
        free(tx_text);
        tx_text = NULL;
        tx_on = false;
        return false;
    }
    pthread_detach(tx_thread);
    return true;
}

void olivia_tx_stop(void) {
    if (tx_on) tx_stop_req = true;
}

void olivia_tx_abort(void) {
    if (tx_on) tx_abort_req = true;
}

bool olivia_tx_is_on(void) {
    return tx_on;
}
