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

/* FT8 and FT4 go out from 0.5 s into the slot, where the far end reads DT 0, as WSJT-X sends. Started when the slot's
 * last decode and the radio let it, anywhere up to 1.5 s in, the far end read our DT move from one transmission to
 * the next. A fixed lead of the keying and the audio path (PulseAudio, the codec, the base) still adds to it: measured
 * on the air, it comes off here */
#define TX_START_NS     500000000LL
#define FADE_IN_HZ      200     /* a waveform cut into starts with a 1/200 s (5 ms) ramp: no click on the air */

/* The waveform handed to tx_player_stream, lined up with the slot at its first block */
typedef struct {
    int16_t    *next;
    uint32_t    left;
    uint32_t    rate;
    float       slot;   /* s */
    bool        lined_up;
} slot_tx_t;

/* Called keyed, before the first block: early, wait for 0.5 s into the slot (at most 0.5 s, well under tx_player's
 * 3 s stall watchdog); late, leave out the part of the waveform already past, so the rest goes where it belongs. The
 * wait is no audio at all, not silence: tx_level counts the blocks played, and blocks of zeros would read ALC 0 and
 * raise the drive */
static void line_up(slot_tx_t *t) {
    struct timespec now;
    int64_t         slot_ns = llround(t->slot * 1e9);
    int64_t         into;

    clock_gettime(CLOCK_REALTIME, &now);
    into = ftx_slot_offset_ns(now.tv_sec, now.tv_nsec, t->slot);
    if (into < 0) into += slot_ns;  /* from the slot's start, 0 .. slot */

    if (into < TX_START_NS) {
        struct timespec d = {.tv_sec = 0, .tv_nsec = (long)(TX_START_NS - into)};

        while (nanosleep(&d, &d) != 0 && errno == EINTR) {
        }
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

/* The FT8/FT4/FT2 message as audio, sent by tx_player.c (keying, level, power cap, VFO). FT8 and FT4 lined up with
 * the slot; FT2 from its first sample as soon as it is keyed: it starts before the slot's last decode (audio_worker.c)
 * and must end inside its 3.75 s */
bool tx_worker_run(const char    *tx_text,
                   int32_t        audio_sample_rate,
                   float          base_gain_offset,
                   tx_abort_fn_t  abort_check,
                   void          *abort_check_ctx) {
    int16_t *samples   = NULL;
    uint32_t n_samples = 0;
    int      proto     = param_i_get(cfg_ft8_protocol);

    bool made = ftx_worker_generate_tx_samples(tx_text, TX_PLAYER_AUDIO_HZ, (uint32_t)audio_sample_rate,
                                               &samples, &n_samples);
    bool sent_all = true;   /* no audio made: nothing sent, which is not a stop */

    if (made) {
        tx_info_set_length(ftx_slot_time(proto));
        if (proto == (int)FTX_PROTOCOL_FT2) {
            sent_all = tx_player_play(samples, n_samples, (int32_t)params.ft8_tx_freq.x, base_gain_offset,
                                      abort_check, abort_check_ctx);
        } else {
            slot_tx_t t = {.next = samples, .left = n_samples, .rate = (uint32_t)audio_sample_rate,
                           .slot = ftx_slot_time(proto)};

            sent_all = tx_player_stream(slot_fill, &t, (int32_t)params.ft8_tx_freq.x, base_gain_offset,
                                        abort_check, abort_check_ctx);
        }
        free(samples);
    }
    return sent_all;
}
