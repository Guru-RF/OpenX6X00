/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - TUNE for an external tuner (#85): a low-power carrier from any mode or app, for a tuner of its own
 *  at the antenna (an LDG, an SGC) to match on, without leaving the app or changing the frequency or the mode. Not
 *  the radio's own ATU, which tunes itself (radio_start_atu); this works with it off.
 *
 *  The key action (Settings > Keys: a front key held, the hand mic's F keys), the Tune button of the digital apps
 *  and the browser Panel start it, and the same again stops it. Its power and time limit are settings (Settings >
 *  TX Cal > EXTERNAL TUNER); a change of frequency, mode, band, VFO or split stops it, and so does the end of the
 *  Panel's lease (remote_server.c). The transmission itself, and what it puts back: tune_tx.c.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Who started a tune: the radio's keys and buttons, or the remote server's client TUNE_BY_PANEL + n */
#define TUNE_BY_RADIO   0
#define TUNE_BY_PANEL   1

/* Once the settings are up; LVGL thread */
void ext_tune_init(void);

/* A tune for `who`: NULL when it is on the air (or was already), else why not, which the screen says too. LVGL thread */
const char *ext_tune_start(int who);

/* The key action and the apps' button: a tune, or the one on the air stopped. LVGL thread */
void ext_tune_toggle(void);

/* The tune stopped at its next block of audio, faded out, whoever started it. Any thread */
void ext_tune_stop(void);

/* The same if `who` started it: true then. gone: its page went (the Panel's lease ran out, the connection closed),
 * "TUNE stopped: browser gone" on the screen; else the page asked, "TUNE stopped" as for the key. Any thread */
bool ext_tune_stop_owner(int who, bool gone);

/* A tune is on the air, or about to be. Any thread */
bool ext_tune_on(void);

/* Why the radio would not tune now, NULL: it would. Any thread: the Panel asks before it queues a start */
const char *ext_tune_refusal(void);

/* The running tune's time limit, else the setting's, s. Any thread */
int ext_tune_limit_s(void);

#ifdef __cplusplus
}
#endif
