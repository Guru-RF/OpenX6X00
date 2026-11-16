/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Zello app's session (zello.c): the helper on a channel, the channel's audio, talking, the Simplex
 *  repeater. Apart from the window (dialog_zello.c): with the link on it stays when the window closes.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ZELLO_OFF, ZELLO_CONNECTING, ZELLO_ONLINE, ZELLO_OFFLINE, ZELLO_RETRY, ZELLO_ERROR } zello_conn_t;

typedef enum {
    ZELLO_LINK_SHOW_IDLE,
    ZELLO_LINK_SHOW_TX,         /* a message from the channel on the air */
    ZELLO_LINK_SHOW_ID,         /* the CW ID on the air */
    ZELLO_LINK_SHOW_TO_ZELLO,   /* the receiver to the channel */
} zello_link_show_t;

typedef struct {
    zello_conn_t      state;
    char              reason[96];
    int               retry_s;
    int               users;            /* -1: not known */
    char              talker[64];
    char              channel[64];
    bool              on;               /* on a channel or about to be (the helper or its start) */
    bool              talking;          /* the user's message wanted */
    bool              talk_bt;          /* ... from the Bluetooth mic */
    bool              btmic;            /* the helper sees a Bluetooth mic */
    bool              link;             /* the Simplex repeater on: a direction switched on, on a channel */
    bool              to_rf_stopped;    /* Zello to RF stopped by the radio (SWR, refused there) */
    zello_link_show_t link_show;
} zello_status_t;

/* GUI thread */
void zello_status(zello_status_t *out);
/* The channel: the helper started on it (another one ended first). False: it could not start */
bool zello_connect(const char *channel);
void zello_disconnect(void);
/* The window opened, and closed: the session ends then, unless the Simplex repeater is on */
void zello_window_opened(void);
void zello_window_closed(void);
/* A message from the user: Talk, or the PTT key with the app open */
void zello_talk_start(bool by_ptt);
void zello_talk_stop(void);
bool zello_talk_by_ptt(void);
/* The radio's PTT key (screen_main.c through dialog_zello_ptt_key), a keypad_state_t: true when Zello takes it. A
 * press stops the link's transmission whoever gets it: the operator's key wins */
bool zello_ptt_key(int state, bool app_open);
/* Zello to RF switched on again: one the radio stopped is armed again */
void zello_link_rearm_rf(void);
/* "No audio: talking on Zello uses the hand mic": once a message, from the status timer */
bool zello_mic_quiet(void);

/* The audio thread (dsp.cpp): the hand mic while the user's message from it is sent; the receiver's audio for the
 * link's squelch and its messages to Zello */
bool zello_mic(void);
void zello_put_mic(size_t nsamples, const int16_t *samples);
void zello_put_rx(size_t nsamples, const int16_t *samples);

#ifdef __cplusplus
}
#endif
