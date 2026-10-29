/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "station.h"

#include "band_plan.h"
#include "textarea_window.h"
#include "params/params.h"
#include "qth/qth.h"
#include "msg.h"
#include "gps.h"

#include <ft8lib/encode.h>
#include <ft8lib/decode.h>

#include <stdio.h>
#include <string.h>

static bool check_ftx_msg_encoding(const char *text) {
    ftx_message_rc_t rc;
    ftx_message_t msg;
    char decoded[128];
    rc = ftx_message_encode(&msg, NULL, text);
    if (rc != FTX_MESSAGE_RC_OK) {
        return false;
    }
    rc = ftx_message_decode(&msg, NULL, decoded);
    if (rc != FTX_MESSAGE_RC_OK) {
        return false;
    }
    return strcmp(text, decoded) == 0;

}

/* A keyboard without Caps Lock types lowercase: taken as capitals, not dropped
 * (from randal007's upstream PR #281) */
static void callsign_insert_cb(lv_event_t *e) {
    const char *in = lv_event_get_param(e);
    static char up[8];
    size_t      n = strlen(in);
    bool        lower = false;

    if (n >= sizeof(up)) return;
    for (size_t i = 0; i <= n; i++) {
        up[i] = in[i] >= 'a' && in[i] <= 'z' ? (char)(in[i] - 'a' + 'A') : in[i];
        lower |= up[i] != in[i];
    }
    if (lower) lv_textarea_set_insert_replace(lv_event_get_target(e), up);
}

void station_callsign_edit(lv_obj_t *text) {
    lv_obj_add_event_cb(text, callsign_insert_cb, LV_EVENT_INSERT, NULL);
    lv_textarea_set_accepted_chars(text,
        "0123456789/"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    );

    lv_textarea_set_max_length(text, sizeof(params.callsign.x) - 1);
    lv_textarea_set_placeholder_text(text, "Callsign");

    textarea_window_set(params.callsign.x);
}

bool station_callsign_save(const char *callsign) {
    char buf[256];

    // Try encode
    snprintf(buf, sizeof(buf), "CQ %s AA00", callsign);
    if (!check_ftx_msg_encoding(buf)) {
        // Try without locator
        snprintf(buf, sizeof(buf), "CQ %s", callsign);
        if (!check_ftx_msg_encoding(buf)) {
            msg_schedule_text_fmt("Unsupported callsign (too long)");
            return false;
        } else {
            msg_schedule_text_fmt("Callsign is long, QTH will be omitted");
        }
    }
    params_str_set(&params.callsign, callsign);
    band_plan_follow();     /* Auto without a locator goes by the callsign's country */
    return true;
}

void station_qth_edit(lv_obj_t *text) {
    lv_textarea_set_accepted_chars(text,
        "0123456789"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    );

    lv_textarea_set_max_length(text, 6);
    lv_textarea_set_placeholder_text(text, "QTH Grid");

    textarea_window_set(params.qth.x);
}

bool station_qth_save(const char *qth) {
    if (!qth_grid_check(qth)) {
        msg_update_text_fmt("Incorrect QTH Grid");
        return false;
    }
    params_str_set(&params.qth, qth);
    band_plan_follow();     /* Auto: the region of the new locator */
    return true;
}

const char *station_qth(void) {
    static char gps_qth[7];
    char        grid[9];
    double      lat, lon;
    int         age;

    /* A current fix, as for the GPS icon. The QTH set is never changed: it is back as soon
     * as the fix is gone */
    if (gps_last_fix(&lat, &lon, &age) && age < 10) {
        qth_pos_to_str(lat, lon, grid);
        snprintf(gps_qth, sizeof(gps_qth), "%s", grid);
        return gps_qth;
    }
    return params.qth.x;
}
