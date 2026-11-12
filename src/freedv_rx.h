/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  FreeDV RADE V1, receive only: the receiver's audio in, the decoded speech to the
 *  Bluetooth headphones. No LVGL in here, so tools/freedv_test.c can run it on a PC.
 *
 *  Threads (none of them is the audio thread, which must not block):
 *    audio thread   freedv_rx_put_audio(): 11025 Hz to 8000 Hz, into a ring
 *    decode thread  rade_rx(): the ring to speech features (120 ms of modem per frame)
 *    synth thread   FARGAN: features to 16 kHz speech, to 44100 Hz, into a ring
 *    play thread    the ring to the Bluetooth headphones (audio_play_bt_begin)
 *  and, set with freedv_rx_set_tap, the speech at FREEDV_RX_TAP_RATE to the TCI clients and the browser
 *  (tci_server_speech_write), from the synth thread.
 *
 *  Built with RADE_ENABLED (CMake: -DRADE_PREFIX=..., tools/rade/build.sh). Without it every
 *  function is there and does nothing, freedv_rx_available() says false.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREEDV_RX_IN_RATE 11025      /* what dialog_t.audio_cb delivers */
#define FREEDV_RX_TAP_RATE 24000     /* the speech to the tap: the TCI server's rate */

/* Where else the speech goes: returns how many samples it took */
typedef size_t (*freedv_rx_tap_t)(const float *x, size_t n);

typedef struct {
    bool     running;
    bool     sync;           /* the modem is locked */
    float    snr_db;         /* 3 kHz SNR estimate, 0 without sync */
    float    foff_hz;        /* frequency offset, 0 without sync */
    int      level_pct;      /* input peak of the last 100 ms, 0..100 (100 = full scale, clipping) */
    bool     audio_out;      /* the headphones are there and the speech goes to them */
    bool     tap_out;        /* the speech goes to the tap */
    uint32_t eoo_count;      /* End-of-Over frames seen */
    char     callsign[9];    /* the callsign the last End-of-Over carried (8 characters, trimmed), empty if none */
    uint32_t in_dropped;     /* input samples lost, the decoder was too slow */
    uint32_t speech_dropped; /* speech samples lost, the player was too slow */
    uint32_t underruns;      /* the stream ran dry in the middle of a talk burst (a gap in the speech) */
    int      decode_ms_max;  /* slowest rade_rx() since start; the budget is 120 */
    int      synth_ms_max;   /* slowest 4 FARGAN frames since start; same budget, in parallel */
} freedv_rx_stats_t;

/* Was RADE built in */
bool freedv_rx_available(void);

/* Opens the modem and starts the threads (LVGL thread). False: not built in, or no memory */
bool freedv_rx_start(void);

/* Stops and joins the threads, frees everything (LVGL thread) */
void freedv_rx_stop(void);

/* The receiver's audio, float +-1.0 at FREEDV_RX_IN_RATE (audio thread; never blocks) */
void freedv_rx_put_audio(unsigned int count, const float *samples);

/* The tap for the speech, NULL none; set before freedv_rx_start (LVGL thread) */
void freedv_rx_set_tap(freedv_rx_tap_t tap);

/* A copy of the numbers for the UI */
void freedv_rx_get_stats(freedv_rx_stats_t *out);

#ifdef __cplusplus
}
#endif
