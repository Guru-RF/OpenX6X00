/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 TX player
 *
 *  Copyright (c) 2026
 */

/*
 *  Synthesises an FT8/FT4 TX waveform from tx_text and pushes it to the
 *  audio output with per-block ALC-based gain correction. Blocks for the
 *  duration of one slot, so it is invoked from the decode thread between
 *  RX frames.
 *
 *  The caller supplies an abort-check callback that the worker polls each
 *  block, so dialog state (e.g. dialog_ft8.c's ft8_state_t) can preempt
 *  TX without this module owning that state itself.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "../tx_player.h"   /* tx_abort_fn_t, the caller's stop */

typedef enum {
    TX_WORKER_SENT,     /* to its end, or nothing made to send (sample generation failed) */
    TX_WORKER_STOPPED,  /* the abort callback fired, or the player did not key (the radio on the air already) */
    TX_WORKER_LATE,     /* FT8/FT4 not keyed: so late into its slot that more than a fifth would be left out */
} tx_worker_rc_t;

/* Transmit tx_text */
tx_worker_rc_t tx_worker_run(const char    *tx_text,
                             int32_t        audio_sample_rate,
                             float          base_gain_offset,
                             tx_abort_fn_t  abort_check,
                             void          *abort_check_ctx);

#ifdef __cplusplus
}
#endif
