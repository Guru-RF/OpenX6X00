/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - TCI server: the radio on the network as a TCI v2.0 transceiver
 *  (Expert Electronics' protocol, the one AetherSDR serves and the VFO-Knob speaks), over a
 *  WebSocket on TCP 50001. Clients tune, set the mode, filter, AGC, RIT and drive, key the
 *  transmitter, read the S-meter and the TX meters, take the RX audio and send TX audio.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "board.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TCI_CONF
#define TCI_CONF         BOARD_DATA "/tci.conf"   /* enabled=0|1, port=NNNN */
#endif
#define TCI_DEFAULT_PORT 50001

/* Reads the configuration and starts the server thread (listening when enabled) */
void tci_server_init(void);

bool tci_server_get_enabled(void);
/* Saves the configuration; the server starts or stops listening */
void tci_server_set_enabled(bool on);
int  tci_server_port(void);

/* What a TCI client may do (trust=operate|view in TCI_CONF): operate, as always, or only listen
 * and follow (R3) */
bool tci_server_get_trust_operate(void);
void tci_server_set_trust_operate(bool on);

/* Connected clients, 0 when none */
int tci_server_clients(void);

/* The RADE app (dialog_freedv.c): while on, the clients' RX audio is the decoded speech written with
 * tci_server_speech_write in place of the receiver's (the modem's own signal is only a hiss), silence between the
 * overs. The browser's Panel and Remote pages get their sound this way too */
#define TCI_SPEECH_RATE  24000
void   tci_server_speech_on(bool on);
/* The speech, float +-1.0 at TCI_SPEECH_RATE, from any one thread; what does not fit is dropped (returns the count
 * taken) */
size_t tci_server_speech_write(const float *x, size_t n);
/* Clients that take the RX audio now */
int    tci_server_audio_clients(void);
/* The address of the first client ("192.168.1.20"), NULL when none; a static buffer */
const char *tci_server_client(void);

#ifdef __cplusplus
}
#endif
