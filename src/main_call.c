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
#include "station.h"

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>

#define POLL_MS 2000

SubjectInt *main_call_subject;

static char shown[sizeof(params.callsign.x)];
static char loc[8];        /* the locator beside it (#343) */
static bool loc_gps;
static int  changes;

static void check(void) {
    char        now[sizeof(shown)], now_loc[sizeof(loc)];
    bool        on  = param_i_get(cfg_callsign_show);
    const char *qth = on ? station_qth() : "";
    bool        gps = on && qth != params.qth.x;

    snprintf(now, sizeof(now), "%s", on ? params.callsign.x : "");
    snprintf(now_loc, sizeof(now_loc), "%s", qth);
    if (strcmp(now, shown) == 0 && strcmp(now_loc, loc) == 0 && gps == loc_gps) return;
    strcpy(shown, now);
    strcpy(loc, now_loc);
    loc_gps = gps;
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

const char *main_locator(bool *gps) {
    if (gps) *gps = loc_gps;
    return loc;
}
