/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Zello app's session (zello.c): the helper on a channel, in the internet voice apps' session
 *  (voice_net.h), which plays the channel's audio, sends the user's messages and runs the Simplex repeater. Apart
 *  from the window (dialog_zello.c): with the link on it stays when the window closes.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include "voice_net.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ZELLO_OFF, ZELLO_CONNECTING, ZELLO_ONLINE, ZELLO_OFFLINE, ZELLO_RETRY, ZELLO_ERROR } zello_conn_t;

typedef struct {
    zello_conn_t state;
    char         reason[96];
    int          retry_s;
    int          users;             /* -1: not known */
    char         talker[64];
    char         channel[64];
    bool         on;                /* on a channel or about to be (the helper or its start) */
    bool         talking;           /* the user's message wanted */
    bool         talk_bt;           /* ... from the Bluetooth mic */
    bool         btmic;             /* the helper sees a Bluetooth mic */
    bool         link;              /* the Simplex repeater on: a direction switched on, on a channel */
    bool         to_rf_stopped;     /* Zello to RF stopped by the radio (SWR, refused there) */
    bool         off_band;          /* outside the amateur bands: Zello to RF keys nothing */
    bool         rx_off_band;       /* the receiver outside them: RF to Zello sends nothing */
    bool         rx_outside;        /* ... but TX outside ham bands is on: it sends, with a warning */
    vnet_show_t  link_show;
} zello_status_t;

/* The app in the session's terms (voice_net.h) */
extern const vnet_app_t zello_app;

/* GUI thread */
void zello_status(zello_status_t *out);
/* The channel: the helper started on it (another one ended first). False: it could not start */
bool zello_connect(const char *channel);
void zello_disconnect(void);
/* The window opened, and closed: the session ends then, unless the Simplex repeater is on */
void zello_window_opened(void);
void zello_window_closed(void);
/* A message from the user: Talk, or the PTT key with the app open (vnet_ptt_key) */
void zello_talk_start(bool by_ptt);
void zello_talk_stop(void);
/* Zello to RF switched on again: one the radio stopped is armed again */
void zello_link_rearm_rf(void);
/* "No audio: talking on Zello uses the hand mic": once a message, from the status timer */
bool zello_mic_quiet(void);

#ifdef __cplusplus
}
#endif
