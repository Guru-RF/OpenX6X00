/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - TUNE for an external tuner: the transmission (tune_tx.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "tune_tx.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "audio.h"          /* AUDIO_PLAY_RATE */
#include "radio.h"          /* radio_set_tune, radio_set_tx_mode, radio_get_modem */
#include "radio_types.h"
#include "tx_meters.h"      /* tx_info_set_tune: TUNE on the TX panel and the apps' strip */
#include "tx_player.h"

#define TONE_AMP    (32767.0f * 0.8f)           /* FT8's: the level the apps share fits it */
#define FADE        (AUDIO_PLAY_RATE / 100)     /* 10 ms in and out: no clicks beside the carrier */

static uint64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);    /* radio.c's get_time() */
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

tune_refusal_t tune_refusal(const tune_ask_t *a) {
    if (!a->tx_freq_ok) return TUNE_NO_TX;
    if (a->on_air) return TUNE_ON_AIR;
    if (a->winlink) return TUNE_WINLINK;
    return TUNE_GO;
}

/* USB-D everywhere: the tone at TX_PLAYER_AUDIO_HZ with the VFO TX_PLAYER_AUDIO_HZ under the TX frequency (tx_player)
 * puts the carrier on it only in the upper sideband, and without the voice modes' processing; an app is in USB-D
 * already */
void tune_plan(tune_plan_t *p, int mode, int32_t app_hz, float pwr_w, float top_w, int limit_s) {
    float top = top_w < TUNE_PWR_MAX_W ? top_w : TUNE_PWR_MAX_W;

    p->tx_mode   = mode == radio_mode_usb_dig ? -1 : radio_mode_usb_dig;
    p->offset_hz = app_hz > 0 ? app_hz : 0;
    p->pwr_w     = pwr_w > top ? top : pwr_w < TUNE_PWR_MIN_W ? TUNE_PWR_MIN_W : pwr_w;
    if (limit_s < TUNE_TIME_MIN_S) limit_s = TUNE_TIME_MIN_S;
    if (limit_s > TUNE_TIME_MAX_S) limit_s = TUNE_TIME_MAX_S;
    p->limit_ms  = (uint32_t)limit_s * 1000;
}

float tune_swr_limit(const tune_guard_t *t, float pwr_w, float setting, uint64_t now_ms) {
    uint64_t age = now_ms > t->start_ms ? now_ms - t->start_ms : 0;   /* the radio thread read its clock first */

    if (t->pwr_w <= 0.0f || age > (uint64_t)t->limit_ms + TUNE_GRACE_MS || pwr_w > TUNE_OWN_W(t->pwr_w))
        return setting;
    return 0.0f;
}

/* The tone, block by block for tx_player_stream */
typedef struct {
    const tune_plan_t *p;
    tune_stop_fn_t     stop;
    void              *ctx;
    uint64_t           start_ms;
    double             phase;
    uint32_t           made;    /* samples so far */
    int                fade;    /* samples of the fade-out made, -1 not fading */
    tune_end_t         end;
    bool               keyed;   /* the radio went on the air for it */
} tone_t;

static size_t tone_fill(int16_t *buf, size_t n, void *arg) {
    tone_t       *t = arg;
    const double  w = 2.0 * M_PI * TX_PLAYER_AUDIO_HZ / AUDIO_PLAY_RATE;
    size_t        i;

    t->keyed = t->keyed || radio_get_modem();
    if (t->fade < 0 && t->stop(t->ctx)) {
        t->end  = TUNE_END_STOP;
        t->fade = 0;
    } else if (t->fade < 0 && now_ms() - t->start_ms >= t->p->limit_ms) {
        t->end  = TUNE_END_LIMIT;
        t->fade = 0;
    }
    for (i = 0; i < n && t->fade < FADE; i++) {
        float env = 1.0f;

        if (t->made < FADE) env = 0.5f - 0.5f * cosf((float)M_PI * t->made / FADE);
        if (t->fade >= 0) env *= 0.5f + 0.5f * cosf((float)M_PI * t->fade++ / FADE);
        buf[i] = (int16_t)lrintf(TONE_AMP * env * (float)sin(t->phase));
        t->phase += w;
        if (t->phase > 2.0 * M_PI) t->phase -= 2.0 * M_PI;
        t->made++;
    }
    return i;   /* 0 once faded out: tx_player plays the rest and unkeys */
}

tune_end_t tune_tx_run(const tune_plan_t *p, tune_stop_fn_t stop, void *ctx) {
    tone_t t = {.p = p, .stop = stop, .ctx = ctx, .fade = -1, .end = TUNE_END_CUT};

    /* The player held from before the radio is set up for the tune until it is back (tx_player_claim). An app's slot
     * that had started and not keyed yet went out at what the refused tune put back (WSPR's beacon at PWR's 5 W, not
     * its own 0.2 W), and one that started as the tune's audio ended keyed at the tune's power with the SWR protection
     * aside. On the air since it was asked (PTT, TCI): nothing is touched, the other's power least of all. tx_player
     * looks again as it keys */
    if (!tx_player_claim()) {
        fprintf(stderr, "tune_tx: another transmission is under way, no tune\n");
        return TUNE_END_REFUSED;
    }
    if (radio_get_modem() || radio_get_state() != RADIO_RX) {
        tx_player_release();
        return TUNE_END_REFUSED;
    }
    t.start_ms = now_ms();
    radio_set_tune(p->pwr_w, p->limit_ms);      /* from now on the power at most the tune's */
    if (p->tx_mode >= 0) radio_set_tx_mode(p->tx_mode);
    tx_info_set_tune(p->pwr_w, p->limit_ms / 1000.0f);
    /* Its own power, on this thread: tx_player keeps no level from it, the one the apps share stays */
    tx_player_set_pwr(p->pwr_w);

    bool whole = tx_player_stream(tone_fill, &t, p->offset_hz, tx_player_base_gain_offset(), NULL, NULL);

    tx_player_set_pwr(0.0f);
    if (p->tx_mode >= 0) radio_set_tx_mode(-1);     /* after the unkey: tx_player waited for the last block */
    radio_set_tune(0.0f, 0);
    tx_info_set_tune(0.0f, 0.0f);
    tx_player_release();
    if (!t.keyed) return TUNE_END_REFUSED;
    return whole ? t.end : TUNE_END_CUT;
}
