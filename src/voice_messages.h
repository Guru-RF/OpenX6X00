/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - voice keyer messages
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "app_window.h"

#include <stddef.h>
#include <stdint.h>

/* What the keyer is doing: dsp.cpp feeds the microphone while it records */
typedef enum { MSG_VOICE_OFF, MSG_VOICE_RECORD, MSG_VOICE_PLAY } msg_voice_state_t;

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_msg_voice;

msg_voice_state_t dialog_msg_voice_get_state();
void dialog_msg_voice_put_audio_samples(size_t nsamples, int16_t *samples);
/* Front PTT key: a message playing or on air stops */
void dialog_msg_voice_ptt_pressed(void);

/* A message by name in /mnt/msg, without the MSG window (Field Ops' voice CQ):
 * rec / send start or stop and return true while recording / sending;
 * the caller polls (closing: stop and wait) while it is open */
bool msg_voice_rec_toggle(const char *name);
bool msg_voice_send_toggle(const char *name);
bool msg_voice_exists(const char *name);
void msg_voice_poll(bool closing);

#ifdef __cplusplus
}
#endif
