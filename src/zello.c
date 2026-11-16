/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Zello app's session: rootfs/usr/sbin/x6100-zello on a channel, started from the app (dialog_zello.c)
 *  in the internet voice apps' session (voice_net.c), which plays the channel, sends the user's messages and runs the
 *  Simplex repeater under main's TX rules. Here: the helper's command line and its lines, in Zello's words.
 *
 *  The helper's lines (rootfs/usr/sbin/x6100-zello has the whole exchange): state connecting | online | offline <why> |
 *  retry <s> <why>, error <why> (it gave up), channel <users>, talker [<name>], talk on | off | refused <why>,
 *  btmic on | off; to it: talk start [bt], talk stop, talkvia bt | mic.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "zello.h"

#include "cfg/cfg_api.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef X6100_SBIN
#define X6100_SBIN "/usr/sbin"
#endif

/* What the helper said, from the play thread to the screen */
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    zello_conn_t state;
    char         reason[96];
    int          retry_s;
    int          users;
    char         talker[64];
} st;

/* GUI thread */
static char cur_channel[64];
static int  via_sent = -1;      /* the talkvia last sent to this helper: 1 bt, 0 mic, -1 none yet */

static char *const *z_argv(void) {
    static char *argv[] = {X6100_SBIN "/x6100-zello", "run", cur_channel, NULL};

    return cur_channel[0] ? argv : NULL;
}

static void z_reset(void) {
    pthread_mutex_lock(&st_lock);
    st.state     = ZELLO_CONNECTING;
    st.reason[0] = st.talker[0] = 0;
    st.users     = -1;
    pthread_mutex_unlock(&st_lock);
    via_sent = -1;
}

/* A line of the helper's (play thread) */
static void z_line(char *line) {
    pthread_mutex_lock(&st_lock);
    if (!strcmp(line, "state connecting")) {
        st.state = ZELLO_CONNECTING;
    } else if (!strcmp(line, "state online")) {
        st.state = ZELLO_ONLINE;
    } else if (!strncmp(line, "state offline ", 14)) {
        st.state = ZELLO_OFFLINE;
        snprintf(st.reason, sizeof(st.reason), "%s", line + 14);
    } else if (!strncmp(line, "state retry ", 12)) {
        char *why = NULL;

        st.state   = ZELLO_RETRY;
        st.retry_s = (int)strtol(line + 12, &why, 10);
        snprintf(st.reason, sizeof(st.reason), "%s", why && *why == ' ' ? why + 1 : "");
    } else if (!strncmp(line, "error ", 6)) {
        st.state = ZELLO_ERROR;
        snprintf(st.reason, sizeof(st.reason), "%s", line + 6);
        vnet_gave_up();
    } else if (!strncmp(line, "channel ", 8)) {
        st.users = line[8] == '-' ? -1 : atoi(line + 8);
    } else if (!strncmp(line, "talker", 6)) {
        snprintf(st.talker, sizeof(st.talker), "%s", line[6] == ' ' ? line + 7 : "");
        vnet_set_talker(st.talker[0] != 0);
    } else if (!strcmp(line, "btmic on") || !strcmp(line, "btmic off")) {
        vnet_set_btmic(line[7] == 'n');
    } else if (!strcmp(line, "talk on")) {
        vnet_talk_event(VNET_TALK_ON, NULL);
    } else if (!strcmp(line, "talk off")) {
        vnet_talk_event(VNET_TALK_OFF, NULL);
    } else if (!strncmp(line, "talk refused ", 13)) {
        vnet_talk_event(VNET_TALK_REFUSED, line + 13);
    }
    vnet_set_connected(st.state == ZELLO_ONLINE);
    pthread_mutex_unlock(&st_lock);
}

/* A message's start or end, the user's or the link's from the receiver: the helper records a Bluetooth mic itself */
static void z_talk(bool start, bool bt, bool link) {
    (void)link;
    vnet_send(!start ? "talk stop\n" : bt ? "talk start bt\n" : "talk start\n");
}

/* The helper told which mic, when that changed */
static void z_tick(void) {
    if (via_sent != param_i_get(cfg_zello_talk_bt)) {
        via_sent = param_i_get(cfg_zello_talk_bt);
        vnet_send(via_sent ? "talkvia bt\n" : "talkvia mic\n");
    }
}

const vnet_app_t zello_app = {
    .name    = "Zello",
    .net     = "Zello",
    .badge   = "ZELLO LINK",
    .tag     = "Z",
    .not_yet = "not on a channel yet",
    .user    = AUDIO_USER_ZELLO,
    .talk_bt = &cfg_zello_talk_bt,
    .to_rf   = &cfg_zello_to_rf,
    .to_net  = &cfg_zello_to_zello,
    .limit   = &cfg_zello_limit,
    .pause   = &cfg_zello_pause,
    .id      = &cfg_zello_id,
    .id_min  = &cfg_zello_id_min,
    .argv    = z_argv,
    .log     = NULL,            /* the helper writes its own, without the secrets (/mnt/app_logs/zello.txt) */
    .reset   = z_reset,
    .line    = z_line,
    .talk    = z_talk,
    .tick    = z_tick,
};

/* ---------- the API ---------- */

void zello_status(zello_status_t *out) {
    vnet_status_t v;

    memset(out, 0, sizeof(*out));
    vnet_status(&zello_app, &v);
    pthread_mutex_lock(&st_lock);
    out->state   = v.on ? st.state : ZELLO_OFF;
    out->users   = st.users;
    out->retry_s = st.retry_s;
    snprintf(out->reason, sizeof(out->reason), "%s", st.reason);
    snprintf(out->talker, sizeof(out->talker), "%s", st.talker);
    pthread_mutex_unlock(&st_lock);
    snprintf(out->channel, sizeof(out->channel), "%s", cur_channel);
    out->on            = v.on;
    out->talking       = v.talking;
    out->talk_bt       = v.talk_bt;
    out->btmic         = v.btmic;
    out->link          = v.on && v.link;
    out->to_rf_stopped = v.to_rf_stopped;
    out->off_band      = v.off_band;
    out->rx_off_band   = v.rx_off_band;
    out->rx_outside    = v.rx_outside;
    out->link_show     = v.show;
}

bool zello_connect(const char *channel) {
    snprintf(cur_channel, sizeof(cur_channel), "%s", channel);
    return vnet_connect(&zello_app);
}

void zello_disconnect(void) {
    vnet_disconnect(&zello_app);
}

void zello_window_opened(void) {
    vnet_window_opened(&zello_app);
}

void zello_window_closed(void) {
    vnet_window_closed(&zello_app);
}

void zello_talk_start(bool by_ptt) {
    vnet_talk_start(&zello_app, by_ptt);
}

void zello_talk_stop(void) {
    vnet_talk_stop(&zello_app);
}

void zello_link_rearm_rf(void) {
    vnet_link_rearm_rf(&zello_app);
}

bool zello_mic_quiet(void) {
    return vnet_mic_quiet(&zello_app);
}
