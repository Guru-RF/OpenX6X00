/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 TX player
 *
 *  Copyright (c) 2026
 *  Slot alignment from SP2KMO's upstream 771df15 (OpenX6100/x6100_gui)
 */

#include "tx_worker.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../cfg/cfg_api.h"     /* cfg_ft8_protocol */
#include "../params/params.h"
#include "../tx_player.h"       /* tx_player_play, tx_player_stream */
#include "../tx_info.h"         /* tx_info_set_length: the slot in the strip */
#include "ft2.h"                /* ftx_slot_time */
#include "utils.h"              /* ftx_slot_offset_ns */
#include "worker.h"

/* FT8 and FT4 go out from 0.5 s into the slot, as WSJT-X sends. Started when the slot's last decode and the radio let
 * it, anywhere up to 1.5 s in, the far end read our DT move from one transmission to the next. The keying and the
 * audio path (PulseAudio, the codec, the base) add a fixed lead, not measured yet: the DT the far end reads on the
 * air, it comes off here, and only then is that DT 0 */
#define TX_START_NS     500000000LL
#define FADE_IN_HZ      200     /* a waveform cut into starts with a 1/200 s (5 ms) ramp: no click on the air */
/* Keyed so late that more than a fifth of the message would be left out (FT8 2.5 s, FT4 1.0 s), it is not sent: its
 * first Costas array and a fifth of its bits gone, the far end may not decode it (not checked on the air) */
#define LATE_SHARE      5

/* The waveform handed to tx_player_stream, lined up with the slot at its first block */
typedef struct {
    int16_t    *next;
    uint32_t    left;
    uint32_t    rate;
    float       slot;   /* s */
    bool        lined_up;
} slot_tx_t;

static void sleep_ns(int64_t ns) {
    struct timespec d = {.tv_sec = (time_t)(ns / 1000000000LL), .tv_nsec = (long)(ns % 1000000000LL)};

    while (nanosleep(&d, &d) != 0 && errno == EINTR) {
    }
}

/* Now from the slot's start by CLOCK_REALTIME (ftx_slot_offset_ns, as Time Sync counts): -0.5 s .. slot - 0.5 s. Up
 * to 0.5 s before a slot is early for it (the clock set back since the slot's tick, ntpd's first sync), not late by
 * almost a whole slot */
static int64_t slot_into_ns(float slot) {
    struct timespec now;
    int64_t         into;

    clock_gettime(CLOCK_REALTIME, &now);
    into = ftx_slot_offset_ns(now.tv_sec, now.tv_nsec, slot);
    return into > -TX_START_NS ? into : into + llround(slot * 1e9);
}

/* Called keyed, before the first block: early, wait for 0.5 s into the slot (at most 0.5 s, tx_worker_run waited
 * for the slot's start unkeyed; well under tx_player's 3 s stall watchdog); late, leave out the part of the waveform
 * already past, so the rest goes where it belongs. The wait is no audio at all, not silence: tx_level counts the
 * blocks played, and blocks of zeros would read ALC 0 and raise the drive */
static void line_up(slot_tx_t *t) {
    int64_t into = slot_into_ns(t->slot);

    if (into < TX_START_NS) {
        sleep_ns(TX_START_NS - into);
        return;
    }

    uint64_t skip = (uint64_t)(into - TX_START_NS) * t->rate / 1000000000ULL;

    if (skip == 0) return;
    if (skip > t->left) skip = t->left;
    t->next += skip;
    t->left -= (uint32_t)skip;

    uint32_t ramp = t->rate / FADE_IN_HZ;

    if (ramp > t->left) ramp = t->left;
    for (uint32_t i = 0; i < ramp; i++) t->next[i] = (int16_t)((int32_t)t->next[i] * (int32_t)i / (int32_t)ramp);
    fprintf(stderr, "ft8: TX %.2f s into the slot, its first %.2f s left out\n", into / 1e9,
            (double)skip / t->rate);
}

static size_t slot_fill(int16_t *buf, size_t n, void *arg) {
    slot_tx_t *t = arg;

    if (!t->lined_up) {
        t->lined_up = true;
        line_up(t);
    }
    if (n > t->left) n = t->left;
    memcpy(buf, t->next, n * sizeof(int16_t));
    t->next += n;
    t->left -= (uint32_t)n;
    return n;
}

/* FT8/FT4 lined up with the slot, decided before keying: too late, not keyed at all; a moment before the slot (the
 * clock set back), its start waited for unkeyed */
static tx_worker_rc_t slot_run(int16_t *samples, uint32_t n_samples, uint32_t rate, int proto, float base_gain_offset,
                               tx_abort_fn_t abort_check, void *abort_check_ctx) {
    slot_tx_t t    = {.next = samples, .left = n_samples, .rate = rate, .slot = ftx_slot_time(proto)};
    int64_t   into = slot_into_ns(t.slot);
    uint64_t  past = into > TX_START_NS ? (uint64_t)(into - TX_START_NS) * rate / 1000000000ULL : 0;

    if (past * LATE_SHARE > n_samples) {
        fprintf(stderr, "ft8: TX %.2f s into the slot, its first %.2f s would be left out: not sent\n", into / 1e9,
                (double)past / rate);
        return TX_WORKER_LATE;
    }
    if (into < 0) {
        sleep_ns(-into);
        if (abort_check && abort_check(abort_check_ctx)) return TX_WORKER_STOPPED;
    }
    tx_info_set_length(t.slot);
    return tx_player_stream(slot_fill, &t, (int32_t)params.ft8_tx_freq.x, base_gain_offset, abort_check,
                            abort_check_ctx)
               ? TX_WORKER_SENT
               : TX_WORKER_STOPPED;
}

/* The FT8/FT4/FT2 message as audio, sent by tx_player.c (keying, level, power cap, VFO). FT8 and FT4 lined up with
 * the slot; FT2 from its first sample as soon as it is keyed: it starts before the slot's last decode (audio_worker.c)
 * and must end inside its 3.75 s */
tx_worker_rc_t tx_worker_run(const char    *tx_text,
                             int32_t        audio_sample_rate,
                             float          base_gain_offset,
                             tx_abort_fn_t  abort_check,
                             void          *abort_check_ctx) {
    int16_t       *samples   = NULL;
    uint32_t       n_samples = 0;
    int            proto     = param_i_get(cfg_ft8_protocol);
    tx_worker_rc_t rc        = TX_WORKER_SENT;  /* no audio made: nothing sent, which is not a stop */

    if (!ftx_worker_generate_tx_samples(tx_text, TX_PLAYER_AUDIO_HZ, (uint32_t)audio_sample_rate, &samples,
                                        &n_samples))
        return rc;
    if (proto == (int)FTX_PROTOCOL_FT2) {
        tx_info_set_length(ftx_slot_time(proto));
        rc = tx_player_play(samples, n_samples, (int32_t)params.ft8_tx_freq.x, base_gain_offset, abort_check,
                            abort_check_ctx)
                 ? TX_WORKER_SENT
                 : TX_WORKER_STOPPED;
    } else {
        rc = slot_run(samples, n_samples, (uint32_t)audio_sample_rate, proto, base_gain_offset, abort_check,
                      abort_check_ctx);
    }
    free(samples);
    return rc;
}
