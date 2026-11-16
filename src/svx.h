/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the SVXConnect app's session (svx.c): svxconnect --stdio on the reflector set on the web page, in the
 *  internet voice apps' session (voice_net.h), which plays the talkgroup, sends the user's overs and runs the Simplex
 *  repeater. Apart from the window (dialog_svx.c): with the link on it stays when the window closes.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include "board.h"
#include "svx_proto.h"
#include "voice_net.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the web page keeps it: the settings, and the key and certificate in pki/ */
#define SVX_DIR   BOARD_DATA "/svxconnect"
#define SVX_CONF  SVX_DIR "/svxconnect.conf"

typedef struct {
    svx_state_t   h;            /* what the helper said */
    vnet_status_t v;            /* the session's side: on, talking, the link */
    bool          set_up;       /* the settings are on the card and the program is there */
} svx_status_t;

/* The app in the session's terms (voice_net.h) */
extern const vnet_app_t svx_app;

/* GUI thread */
void svx_status(svx_status_t *out);
bool svx_set_up(void);
/* The helper started on the reflector (another app's session ended first). False: it could not start */
bool svx_connect(void);
void svx_disconnect(void);
/* The window opened, and closed: the session ends then, unless the Simplex repeater is on */
void svx_window_opened(void);
void svx_window_closed(void);
/* An over from the user: Talk, or the PTT key with the app open (vnet_ptt_key) */
void svx_talk_start(bool by_ptt);
void svx_talk_stop(void);
/* The talkgroup to listen and talk on (0: monitoring only), and the lock that keeps it there */
void svx_select(uint32_t tg);
void svx_lock(bool on);
/* SVX to RF switched on again: one the radio stopped is armed again */
void svx_link_rearm_rf(void);
/* "No audio: talking uses the hand mic": once an over, from the status timer */
bool svx_mic_quiet(void);

#ifdef __cplusplus
}
#endif
