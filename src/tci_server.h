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

/* Connected clients, 0 when none */
int tci_server_clients(void);
/* The address of the first client ("192.168.1.20"), NULL when none; a static buffer */
const char *tci_server_client(void);

#ifdef __cplusplus
}
#endif
