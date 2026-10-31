/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  FreeDV RADE V1 transmitter, "record, then send": the hand mic is recorded and encoded in one
 *  go (Rec), the finished over is played into the transmitter by tx_player (Send). Live speech
 *  is not done: whether Linux hears the mic while the base transmits from the modem input is
 *  not known (TODO.md). No LVGL in here, so tools/freedv_test.c can run it on a PC.
 *
 *  The mic's audio thread (freedv_tx_put_mic) resamples 44100 -> 16000 Hz into a ring and never
 *  blocks. The encoder thread turns 10 ms of speech into LPCNet features, twelve of them into
 *  one rade_tx() modem frame (960 samples at 8 kHz, the real part is what goes out), and keeps
 *  the result in memory. Stopping adds the End-of-Over frame, which carries the callsign.
 *
 *  Built with RADE_ENABLED (see freedv_rx.h); without it every function is there and does nothing.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREEDV_TX_MIC_RATE  44100   /* what the capture delivers (AUDIO_CAPTURE_RATE) */
#define FREEDV_TX_MAX_S     60      /* the longest over */
#define FREEDV_CALL_MAX     8       /* characters of the callsign the End-of-Over carries */

typedef struct {
    bool active;            /* recording */
    bool full;              /* FREEDV_TX_MAX_S reached: stop it */
    bool done;              /* stopped and fully encoded: the over can be sent */
    int  rec_ms;            /* speech recorded */
    int  encoded_ms;        /* speech encoded so far */
    int  level_pct;         /* mic peak since the last call, 0..100 (100 = full scale) */
    int  encode_ms_max;     /* the slowest 12 frames of speech (120 ms) to a modem frame, ms */
    int  over_ms;           /* length of the finished over on the air, with the End-of-Over */
} freedv_tx_stats_t;

bool freedv_tx_available(void);

/* Starts a recording (LVGL thread). callsign goes into the End-of-Over (upper case, the first
 * FREEDV_CALL_MAX characters). False: not built in, no memory, or one is there already */
bool freedv_tx_begin(const char *callsign);

/* The mic's audio, int16 at FREEDV_TX_MIC_RATE (audio thread; never blocks) */
void freedv_tx_put_mic(size_t count, const int16_t *samples);

/* Ends the recording; the encoder finishes in the background (stats.done) */
void freedv_tx_stop(void);

/* Drops the recording and the over, stops the threads (LVGL thread) */
void freedv_tx_discard(void);

/* The finished over as the audio tx_player_stream wants it (tx_fill_fn_t): 44100 Hz int16, 0 at the
 * end. Rewinds with freedv_tx_rewind() to send it again */
size_t freedv_tx_read(int16_t *buf, size_t n, void *ctx);
void   freedv_tx_rewind(void);

void freedv_tx_get_stats(freedv_tx_stats_t *out);

#ifdef __cplusplus
}
#endif
