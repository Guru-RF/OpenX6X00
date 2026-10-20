/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - PSK31 receiver
 *
 *  BPSK31 demodulator: complex downconversion to baseband, narrow
 *  low-pass, decimation to 8 samples per symbol, symbol timing recovery
 *  from the envelope dip that PSK31's shaping produces at every phase
 *  reversal, differential phase detection and Varicode decoding.
 *
 *  Decoded text goes to the PSK31 window through psk_rx_set_text_cb(),
 *  or to the panel when no callback is set.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Rate of the app's audio (AUDIO_CAPTURE_RATE / AUDIO_DECIM). 11025 / 22
 * = 501 Hz after the first decimation for PSK31 (11025 / 11 for PSK63),
 * 16.04 samples per symbol, the working point the filters and the timing
 * loop are tuned for. The window feeds its waterfall from the same audio.
 */
#define PSK_RX_RATE 11025

#ifdef __cplusplus
extern "C" {
#endif

void psk_rx_init(void);

/* 31.25 (PSK31) or 62.5 (PSK63). Restart the receiver afterwards. */
void psk_rx_set_baud(float baud);

/* Enable/disable the receiver. While disabled it consumes nothing. */
void psk_rx_set_active(bool on);
bool psk_rx_is_active(void);

/* Audio at PSK_RX_RATE, from the window's audio callback (and host tests). */
void psk_rx_put_audio_samples(size_t n, float *samples);

/* Re-read the centre frequency (called when the operator retunes). */
void psk_rx_update_center(void);

/* Measured signal quality, for display: estimated frequency error in Hz
 * and a 0..100 lock indicator. */
float psk_rx_freq_error(void);
uint8_t psk_rx_quality(void);

/*
 * Hooks for the PSK window. Both are called from the audio thread with
 * the receiver's lock held, so they must be short and must not call
 * back into this module; hand the work to the UI thread instead.
 *
 * text_cb: decoded characters go here.
 * audio_tap: when set, sees every block of audio the receiver gets,
 *            before it is processed, while the receiver is active.
 *
 * After psk_rx_set_active(false) returns neither is called again.
 */
typedef void (*psk_rx_text_cb_t)(char c);
typedef void (*psk_rx_audio_tap_t)(unsigned int n, const float *samples);

void psk_rx_set_text_cb(psk_rx_text_cb_t cb);
void psk_rx_set_audio_tap(psk_rx_audio_tap_t tap);

#ifdef __cplusplus
}
#endif
