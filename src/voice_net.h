/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the session of the internet voice apps (Zello, SVXConnect), voice_net.c: the app's helper program on its
 *  network, the network's audio on the speaker, talking with the hand mic or a headset, the Simplex repeater
 *  (voice_link.h). One session at a time, whichever app has it: the base's audio path, the speaker, the microphone and
 *  the link are the radio's one each. Each app brings its helper and its words (vnet_app_t) and reads its helper's
 *  lines itself, telling the session what they mean with the vnet_ calls below; zello.c and svx.c are those two.
 *
 *  Apart from the window (dialog_zello.c, dialog_svx.c): with the app's link on the session stays when the window
 *  closes.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include "audio.h"
#include "cfg/parameter_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VNET_SHOW_IDLE,
    VNET_SHOW_TX,               /* a message from the network on the air */
    VNET_SHOW_ID,               /* the CW ID on the air */
    VNET_SHOW_TO_NET,           /* the receiver to the network */
} vnet_show_t;

/* What the helper said of a message: started, ended, or not taken (its words why) */
typedef enum { VNET_TALK_ON, VNET_TALK_OFF, VNET_TALK_REFUSED } vnet_talk_t;

typedef struct {
    const char *name;           /* the app, in its messages: "Zello", "SVXConnect" */
    const char *net;            /* the network in the link's words: "Zello to RF", "RF to SVX" */
    const char *badge;          /* the main screen's while its link is on: "ZELLO LINK" */
    const char *tag;            /* the network in the badge's directions: "Z" for "Z>RF" and "RF>Z" */
    const char *not_yet;        /* Talk before it is connected: "not on a channel yet" */
    audio_user_t user;          /* its stream and claims on the speaker (audio_owner.h): AUDIO_USER_ZELLO, _SVX */
    /* Its settings: the mic to talk with, the link's two directions, its time limit, its pause, its CW ID */
    ParamInt *const *talk_bt, *const *to_rf, *const *to_net, *const *limit, *const *pause, *const *id, *const *id_min;
    /* The helper's command line, argv[0] the program; NULL: it cannot start (GUI thread) */
    char *const *(*argv)(void);
    /* Where the helper's stderr goes, NULL nowhere */
    const char *log;
    /* A new helper is about to start: the app's own state from the start (GUI thread) */
    void (*reset)(void);
    /* A text line of the helper's (the session's play thread): the app's protocol, told with vnet_ */
    void (*line)(char *line);
    /* The helper told to start or end a message: the user's (bt: from a Bluetooth mic), or the link's from the
     * receiver (link). Any thread but the audio one */
    void (*talk)(bool start, bool bt, bool link);
    /* Every TIMER_MS on the GUI thread while the session runs */
    void (*tick)(void);
} vnet_app_t;

typedef struct {
    bool        on;             /* this app's session on or about to be (the helper or its start) */
    bool        connected;      /* on the network */
    bool        talking;        /* the user's message wanted */
    bool        talk_bt;        /* ... from the Bluetooth mic */
    bool        btmic;          /* a Bluetooth mic is there */
    bool        link;           /* the Simplex repeater on: a direction switched on, the session on */
    bool        to_rf_stopped;  /* network to RF stopped by the radio (SWR, refused there) */
    bool        off_band;       /* the radio is outside the amateur bands: the link keys nothing */
    bool        rx_off_band;    /* the receiver is outside them: nothing goes from it to the network */
    bool        rx_outside;     /* ... but TX outside ham bands is on: it goes, with a warning */
    vnet_show_t show;
} vnet_status_t;

/* GUI thread */
void vnet_status(const vnet_app_t *a, vnet_status_t *out);
/* The app whose session is on or about to be, NULL none */
const vnet_app_t *vnet_current(void);
/* The app's session started: its helper (another app's session ends first; a running one of its own too, for a new
 * channel). False: it could not start */
bool vnet_connect(const vnet_app_t *a);
void vnet_disconnect(const vnet_app_t *a);
/* The app's window opened, and closed: the session ends then, unless its Simplex repeater is on */
void vnet_window_opened(const vnet_app_t *a);
void vnet_window_closed(const vnet_app_t *a);
/* A message from the user: Talk, or the PTT key with the app open */
void vnet_talk_start(const vnet_app_t *a, bool by_ptt);
void vnet_talk_stop(const vnet_app_t *a);
bool vnet_talk_by_ptt(void);
/* The radio's PTT key (screen_main.c), a keypad_state_t: true when an app takes it, which it does while its window is
 * open (voice_ptt.h). A press stops the link's transmission whoever gets it: the operator's key wins */
bool vnet_ptt_key(int state);
/* Network to RF switched on again: one the radio stopped is armed again */
void vnet_link_rearm_rf(const vnet_app_t *a);
/* "No audio: talking uses the hand mic": once a message, from the window's timer */
bool vnet_mic_quiet(const vnet_app_t *a);
/* The other app's link would be on with this one: its name, else NULL. One link at a time */
const char *vnet_link_taken(const vnet_app_t *a, const vnet_app_t *other);

/* The app's protocol, from its line handler (the play thread) or any thread but the audio one */
void vnet_set_connected(bool on);
void vnet_set_talker(bool on);          /* the network's message plays: someone talks */
void vnet_talk_event(vnet_talk_t what, const char *why);
void vnet_set_btmic(bool on);
void vnet_gave_up(void);                /* the helper gave up: not started again by itself */
void vnet_notice(const char *text);     /* said once on the screen, from the window's timer */
void vnet_send(const char *line);       /* a line to the helper */
void vnet_send_audio(const int16_t *pcm, int n);    /* the user's message from a Bluetooth mic, 16 kHz */

/* The audio thread (dsp.cpp): the hand mic while the user's message from it is sent; the receiver's audio for the
 * link's squelch and its messages to the network */
bool vnet_mic(void);
void vnet_put_mic(size_t nsamples, const int16_t *samples);
void vnet_put_rx(size_t nsamples, const int16_t *samples);

#ifdef __cplusplus
}
#endif
