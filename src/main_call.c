/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The callsign on the main screen: see main_call.h
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "main_call.h"

#include "cfg/cfg_api.h"
#include "params/params.h"

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>

#define POLL_MS 2000

SubjectInt *main_call_subject;

static char shown[sizeof(params.callsign.x)];
static int  changes;

static void check(void) {
    char now[sizeof(shown)];

    snprintf(now, sizeof(now), "%s", param_i_get(cfg_callsign_show) ? params.callsign.x : "");
    if (strcmp(now, shown) == 0) return;
    strcpy(shown, now);
    subject_i_set(main_call_subject, ++changes);
}

static void poll_cb(lv_timer_t *t) {
    (void)t;
    check();
}

static void switch_cb(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    check();
}

void main_call_init(void) {
    if (main_call_subject) return;
    main_call_subject = subject_i_create(0);
    check();
    subject_subscribe_delayed((Subject *)cfg_callsign_show, switch_cb, NULL);
    lv_timer_create(poll_cb, POLL_MS, NULL);
}

const char *main_call(void) {
    return shown;
}
