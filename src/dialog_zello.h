/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Zello: the radio on a channel of Zello, the push-to-talk voice service on the internet (dialog_zello.c)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include "dialog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_zello;

/* The radio's PTT key (screen_main.c), state a keypad_state_t: true when the app takes it, false when it is the
 * radio's (ptt_key). While the app is open a press talks on the channel and its release ends the message, and the
 * radio's PTT never hears of them; a release goes where its press went (zello_ptt.h) */
bool dialog_zello_ptt_key(int state);

#ifdef __cplusplus
}
#endif
