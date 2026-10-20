/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - PSK31 / PSK63 transmitter
 *
 *  BPSK modulator: Varicode encoding, 31.25 or 62.5 baud, cosine-shaped
 *  envelope through phase reversals (this shaping is what keeps the
 *  occupied bandwidth near 60 / 120 Hz). Streams through tx_player, the
 *  TX path of the other digital apps.
 *
 *  Transmit only; the receiver is psk_rx.c. Audio is generated at
 *  AUDIO_PLAY_RATE.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Centre (audio) frequency of the BPSK carrier, Hz. */
uint16_t psk_tx_get_center(void);
void     psk_tx_set_center(uint16_t hz);

/* Start transmitting text asynchronously. False if already running. */
bool psk_tx_send(const char *text);

/* Request a stop; the current symbol finishes, then the postamble runs. */
void psk_tx_stop(void);

/* Stop now, at the end of the audio block (the window closing). */
void psk_tx_abort(void);

bool psk_tx_is_on(void);

/* 31.25 (PSK31) or 62.5 (PSK63); not changed while transmitting. */
float psk_tx_get_baud(void);
void  psk_tx_set_baud(float baud);


/* Varicode code string for a character, or NULL. Shared with the
 * receiver so the table exists in exactly one place. */
const char *psk_tx_varicode(unsigned char c);

/* The PSK31 window shows what is sent in its own text area: when set,
 * psk_tx_send() hands the text here. Called on the thread that calls
 * psk_tx_send(), which is the UI thread. */
typedef void (*psk_tx_echo_cb_t)(const char *text);
void psk_tx_set_echo_cb(psk_tx_echo_cb_t cb);

#ifdef __cplusplus
}
#endif
