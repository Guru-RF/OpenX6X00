/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Bluetooth headphones: btaudio-ctl runner and "Mute speaker".
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called in the LVGL thread with the output of the command. */
typedef void (*btaudio_done_cb_t)(const char *out, int status, void *user);

/* Mute the speaker (AF volume 0) while headphones are connected. */
void btaudio_init(void);

/* Run "btaudio-ctl <args>" in a worker thread; cb (may be NULL) gets the result. */
void btaudio_run(const char *args, btaudio_done_cb_t cb, void *user);
void sh_run(const char *command, btaudio_done_cb_t cb, void *user);

bool btaudio_mute_get(void);
void btaudio_mute_set(bool on);

/* True while headphones are connected and the speaker is to stay silent. */
bool btaudio_speaker_muted(void);

/* Headphone sink name for local playback while the speaker is silenced. */
bool btaudio_hp_sink(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
