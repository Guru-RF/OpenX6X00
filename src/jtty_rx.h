/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - JTTY receiver
 *
 *  Finds JTTY frames anywhere in the passband, at any time, decodes them
 *  (jtty.c) and joins the frames of a transmission into messages, which
 *  grow frame by frame as they arrive, as in WSJT-X.
 *
 *  The audio is taken from the app's callback and handed to a thread of
 *  its own: a frame can cost tens of milliseconds to decode.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rate of the app's audio (AUDIO_CAPTURE_RATE / AUDIO_DECIM) */
#define JTTY_RX_RATE 11025

typedef struct {
    uint32_t id;            /* same id while a message grows */
    float    freq;          /* audio Hz of the lowest tone */
    int      snr;           /* dB in 2500 Hz, latest frame */
    bool     complete;      /* end-of-message frame received */
    char     text[81];
    char     call[14];      /* the last callsign atom, "" if none */
    int      call_kind;     /* its form, JTTY_CALL_* */
} jtty_rx_msg_t;

/* Called from the receiver's thread: copy and hand over. */
typedef void (*jtty_rx_cb_t)(const jtty_rx_msg_t *m);

/* Start / stop the receiver thread. After stop returns the callback is
 * not called again. */
void jtty_rx_start(jtty_rx_cb_t cb);
void jtty_rx_stop(void);

/* Audio at JTTY_RX_RATE, from the audio thread. */
void jtty_rx_put_audio(size_t n, const float *samples);

/* Wait until everything handed over has been processed (tests). */
void jtty_rx_drain(void);

/* Audio range searched, Hz; and the operator's frequency (lowest tone)
 * with its tolerance: there weaker signals are accepted, as WSJT-X does
 * in its Rx window. */
void jtty_rx_set_range(float lo_hz, float hi_hz);
void jtty_rx_set_freq(float f0_hz, float tol_hz);

/* Forget the audio heard so far (after a transmission). */
void jtty_rx_reset(void);

#ifdef __cplusplus
}
#endif
