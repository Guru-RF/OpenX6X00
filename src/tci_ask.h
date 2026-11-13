/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The radio's question when a TCI client on the network asks to transmit (tci_ask.c): "<client> wants
 *  to transmit", Allow once / Always / No, over whatever is on the screen, answered with the MFK (turn, press), the
 *  keyboard's arrows, Enter and Esc, or a touch. The TCI server (tci_server.c) asks and decides.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL thread: the question for the TCI server's id, about who, open for seconds; the answer goes to
 * tci_server_answer(id, ...). It takes the place of one still open */
void tci_ask_show(uint32_t id, const char *who, int seconds);

/* LVGL thread: what became of question id, a tci_ask_t: the box closes if it is still open, and a note follows no
 * answer in time (TCI_ASK_TIMEOUT), an Always kept (TCI_ASK_ALWAYS) or one with no room (TCI_ASK_FULL). Another
 * question open now stays */
void tci_ask_close(uint32_t id, int why);

/* A question is on the screen */
bool tci_ask_open(void);

#ifdef __cplusplus
}
#endif
