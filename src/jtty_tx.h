/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - JTTY transmitter
 *
 *  Text is packed into frames (jtty.c) and sent as 4-GFSK, BT 2.0, through
 *  tx_player_stream() like the other digital apps. Messages sent while
 *  one is on the air are queued and follow without a gap, as in WSJT-X.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio frequency of the lowest tone, Hz */
uint16_t jtty_tx_get_freq(void);
void     jtty_tx_set_freq(uint16_t hz);

/* Queue a message (profile: JTTY_PROFILE_*). canon (81 bytes) gets the
 * text as it will be received. False if it cannot be encoded or the
 * queue is full. */
bool jtty_tx_send(const char *text, int profile, char *canon);

/* Stop at the end of the frame on the air, dropping the queue. */
void jtty_tx_stop(void);

/* Stop now (the window closing). */
void jtty_tx_abort(void);

bool jtty_tx_is_on(void);

/* Frames still to send, including the one on the air. */
int  jtty_tx_frames_left(void);

#ifdef __cplusplus
}
#endif
