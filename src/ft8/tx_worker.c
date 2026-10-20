/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 TX player
 *
 *  Copyright (c) 2026
 */

#include "tx_worker.h"

#include <stdlib.h>

#include "../cfg/cfg_api.h"     /* cfg_ft8_protocol */
#include "../params/params.h"
#include "../tx_player.h"       /* tx_player_play */
#include "../tx_info.h"         /* tx_info_set_length: the slot in the strip */
#include "ft2.h"                /* ftx_slot_time */
#include "worker.h"

/* The FT8/FT4/FT2 message as audio, sent by tx_player.c (keying, level, power cap, VFO) */
bool tx_worker_run(const char    *tx_text,
                   int32_t        audio_sample_rate,
                   float          base_gain_offset,
                   tx_abort_fn_t  abort_check,
                   void          *abort_check_ctx) {
    int16_t *samples   = NULL;
    uint32_t n_samples = 0;

    bool made = ftx_worker_generate_tx_samples(tx_text, TX_PLAYER_AUDIO_HZ, (uint32_t)audio_sample_rate,
                                               &samples, &n_samples);
    bool sent_all = true;   /* no audio made: nothing sent, which is not a stop */

    if (made) {
        tx_info_set_length(ftx_slot_time(param_i_get(cfg_ft8_protocol)));
        sent_all = tx_player_play(samples, n_samples, (int32_t)params.ft8_tx_freq.x, base_gain_offset,
                                  abort_check, abort_check_ctx);
        free(samples);
    }
    return sent_all;
}
