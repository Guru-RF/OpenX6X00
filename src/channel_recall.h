// SPDX-License-Identifier: LGPL-2.1-or-later
//
// OpenX6X00 - a channel recalled: what is done, and in which order (#346).
//
// A channel may name an app (channel_list.h). An app opens where it was last used (its own
// frequency and mode, RTTY's memory, app_window.c), so tuning first and opening it after left
// the radio on the app's frequency, not the channel's. The order is: the app, when the recall
// may open one and this version knows it; then the frequency; then the mode (a tune into
// another band loads that band's mode, which the channel's replaces); then AGC, PRE and ATT.
//
// with_app is the CHANNELS window's press only: the hand mic's UP/DOWN and the Scanner tune and
// open nothing. The header alone, with the radio behind four calls, so the host test
// (tools/channels) runs the same lines as the GUI.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "channel_list.h"

typedef struct {
    void (*open_app)(const char *app);     /* one of channels_app_id()'s */
    void (*set_freq)(uint32_t hz);
    void (*set_mode)(radio_mode_t mode);
    void (*set_agc)(radio_agc_t agc);
    void (*set_pre)(bool on);
    void (*set_att)(bool on);
} channel_recall_ops_t;

/* True when the app was asked for. c is the caller's own copy: opening an app may read the channel file again */
static inline bool channel_recall(const channel_t *c, bool with_app, const channel_recall_ops_t *ops) {
    bool app = with_app && channels_app_known(c->app);

    if (app) ops->open_app(c->app);
    ops->set_freq(c->freq);
    ops->set_mode(c->mode);
    ops->set_agc(c->agc);
    ops->set_pre(c->pre);
    ops->set_att(c->att);
    return app;
}
