/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - TCI server: the radio on the network as a TCI v2.0 transceiver
 *  (Expert Electronics' protocol, the one AetherSDR serves and the VFO-Knob speaks), over a
 *  WebSocket on TCP 50001. Clients tune, set the mode, filter, AGC, RIT and drive, key the
 *  transmitter, read the S-meter and the TX meters, take the RX audio and send TX audio, and send
 *  CW text to the radio's own keyer at its KEY Speed, which they set too (#179).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "board.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TCI_CONF
#define TCI_CONF         BOARD_DATA "/tci.conf"   /* enabled=0|1, port=NNNN, trust=operate|view, tx_confirm=0|1 */
#endif
#ifndef TCI_ALLOWED
#define TCI_ALLOWED      BOARD_DATA "/tci_allowed.conf"   /* the clients allowed to transmit: ADDR\tNAME\tCREATED\tLAST */
#endif
#define TCI_DEFAULT_PORT 50001
#define TCI_ALLOWED_MAX  16
#define TCI_ASK_S        20       /* a question on the screen unanswered this long is a No */

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

/* A client on the network that asks to transmit, and is not allowed yet, is answered trx:0,false and the radio asks
 * on its screen (tci_ask.h): "<client> wants to transmit", Allow once (while that connection stays open), Always (its
 * address kept in TCI_ALLOWED), No (for that connection); no answer within TCI_ASK_S is a No. Programs on the radio
 * itself, the browser Panel with the key of a paired browser that may transmit, and the clients allowed always are
 * not asked. tx_confirm=0 in TCI_CONF: none is asked, as before. On by default */
bool tci_server_get_tx_confirm(void);
void tci_server_set_tx_confirm(bool on);

/* The operator's answer to the question tci_ask_show() was given id for; one for a question closed since does nothing.
 * Any thread. What became of a question, as tci_ask_close() is told: the answer, TCI_ASK_TIMEOUT (none in time: a
 * No), TCI_ASK_GONE (the client left first), TCI_ASK_FULL (an Always with no room left to keep it: once) */
typedef enum { TCI_ASK_ONCE = 1, TCI_ASK_ALWAYS, TCI_ASK_NO, TCI_ASK_TIMEOUT, TCI_ASK_GONE, TCI_ASK_FULL } tci_ask_t;
void tci_server_answer(uint32_t id, tci_ask_t answer);

/* The clients allowed always: their count, and as "192.168.1.20 (WSJT-X), ..." into buf (n bytes; NULL: the count
 * alone). Any thread */
int  tci_server_allowed(char *buf, size_t n);
/* The i-th one, false past the last: its address, the name it gave, when it last keyed (wall clock, s) and whether a
 * client from there is connected now */
bool tci_server_allowed_get(int i, char *addr, size_t an, char *name, size_t nn, uint64_t *last, bool *online);
/* The one of that address forgotten, every one with NULL: its next start asks again. False: none was there. Any
 * thread */
bool tci_server_forget(const char *addr);

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
