/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Winlink: the radio's side of the Winlink modems and Pat (winlink.c)
 *
 *  The modem, ardopcf (ARDOP), x6x00-mercury (Mercury) or xara (XARA), has no sound card here: it connects
 *  to the GUI on a UNIX socket, hears the receiver through it and sends its audio and its PTT back, and the
 *  GUI keys the transmitter as for its own digital modes. Pat, the mail client, tunes the radio
 *  through a rigctld of the GUI's own on 127.0.0.1. Both listen from the start and do nothing until
 *  a client comes; x6100-winlink starts the chosen modem and Pat when Winlink is on.
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

/* The modem's audio and PTT: SOCK_SEQPACKET, 0600 (ardopcf ... SOCK:/run/ardop.sock, x6x00-mercury
 * SOCK:/run/ardop.sock, xara --audio x6x00 --capture /run/ardop.sock) */
#ifndef WINLINK_ARDOP_SOCK
#ifdef HOST_BUILD
#define WINLINK_ARDOP_SOCK  BOARD_DATA "/run/ardop.sock"
#else
#define WINLINK_ARDOP_SOCK  "/run/ardop.sock"
#endif
#endif

/* The radio's state for the Winlink web page and the supervisor, key=value lines, while a modem is connected */
#ifndef WINLINK_RADIO_FILE
#ifdef HOST_BUILD
#define WINLINK_RADIO_FILE  BOARD_DATA "/run/winlink/radio"
#else
#define WINLINK_RADIO_FILE  "/run/winlink/radio"
#endif
#endif

/* Pat's hamlib rig: TCP on 127.0.0.1 */
#ifndef WINLINK_RIG_PORT
#define WINLINK_RIG_PORT    4532
#endif

/* Starts the modem bridge, the rigctld server and the radio file's writer: once, after audio_init() */
void winlink_init(void);

/* A Winlink connection holds the radio: the modem connected, and keying, or Pat's tune or the modem's last key less
 * than a minute ago (a connection under way, as the keys go by it). Any thread: TUNE for an external tuner asks */
bool winlink_busy(void);

/* APP > Mail's Connect (#322), the LVGL thread. The first press: the radio on a gateway's dial frequency hz, in USB-D
 * as for Pat's QSY, nothing transmitted, so the antenna tuner can be run there before the call. False, and why (n
 * bytes), when the radio may not go there: on the air, an app or LOCK holds the frequency, not one it can tune, or Pat
 * tuned the radio in the last seconds (a connection that begins, before Pat says it is dialing) */
bool winlink_preset(int32_t hz, char *why, size_t n);

/* That first press was for hz and the radio is still there: the next press calls */
bool winlink_preset_at(int32_t hz);

/* No call after all, or (after_call) the connection the second press began is over: back to where the radio was
 * before the first press, unless the dial was turned since. On the air (the modem's last frame), once that is over.
 * With no call from the app, not when Pat tuned there since the first press: a connection from the web mailbox has
 * the radio */
void winlink_preset_back(bool after_call);

#ifdef __cplusplus
}
#endif
