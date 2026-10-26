/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Winlink: the radio's side of ARDOP and Pat (winlink.c)
 *
 *  ardopcf, the ARDOP modem, has no sound card here: it connects to the GUI on a UNIX socket,
 *  hears the receiver through it and sends its audio and its PTT back, and the GUI keys the
 *  transmitter as for its own digital modes. Pat, the mail client, tunes the radio through a
 *  rigctld of the GUI's own on 127.0.0.1. Both listen from the start and do nothing until a
 *  client comes; x6100-winlink starts ardopcf and Pat when Winlink is on.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ardopcf's audio and PTT: SOCK_SEQPACKET, 0600 (ardopcf ... SOCK:/run/ardop.sock) */
#ifndef WINLINK_ARDOP_SOCK
#ifdef HOST_BUILD
#define WINLINK_ARDOP_SOCK  BOARD_DATA "/run/ardop.sock"
#else
#define WINLINK_ARDOP_SOCK  "/run/ardop.sock"
#endif
#endif

/* Pat's hamlib rig: TCP on 127.0.0.1 */
#ifndef WINLINK_RIG_PORT
#define WINLINK_RIG_PORT    4532
#endif

/* Starts the ARDOP bridge and the rigctld server: once, after audio_init() */
void winlink_init(void);

#ifdef __cplusplus
}
#endif
