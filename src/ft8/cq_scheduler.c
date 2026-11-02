/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 CQ scheduler
 *
 *  Copyright (c) 2026
 */

#include "cq_scheduler.h"

#include <stdio.h>
#include <string.h>

#include <ft8lib/message.h>

#include "lvgl/lvgl.h"
#include "msg_encode.h"     /* ftx_call_is_standard */

/* text goes on the air as written: encoded and decoded back the same, "CQ_POTA" read as "CQ POTA". Zeroed first:
 * free text leaves payload[9], the message type, as it was */
static bool goes_out(const char *text) {
    ftx_message_t    msg;
    ftx_message_rc_t rc;
    char             said[FTX_MAX_MESSAGE_LENGTH], heard[FTX_MAX_MESSAGE_LENGTH];

    snprintf(said, sizeof(said), "%s", text);
    if (!strncmp(said, "CQ_", 3)) said[2] = ' ';
    ftx_message_init(&msg);
    rc = ftx_message_encode(&msg, NULL, text);
    if (rc == FTX_MESSAGE_RC_OK) rc = ftx_message_decode(&msg, NULL, heard);
    if (rc != FTX_MESSAGE_RC_OK) {
        LV_LOG_USER("Error %d while encoding '%s'", rc, text);
        return false;
    }
    if (strcmp(said, heard)) {
        LV_LOG_USER("'%s' goes out as '%s'", text, heard);
        return false;
    }
    return true;
}

bool cq_make_message(const char *callsign,
                     const char *qth,
                     const char *cq_mod,
                     char       *out) {
    char buf[FTX_MAX_MESSAGE_LENGTH];

    /* Not a base call (PA/ON6URE, KF0QGT/6Y): a type 4 CQ, no room for a grid or a modifier. ft8_lib 73b0db8
     * leaves the grid out by itself, and refuses "CQ_POTA PA/ON6URE" altogether */
    if (ftx_call_is_standard(callsign)) {
        int n = cq_mod[0] ? snprintf(buf, sizeof(buf), "CQ_%s %s", cq_mod, callsign)
                          : snprintf(buf, sizeof(buf), "CQ %s", callsign);

        if (qth[0] && n > 0 && n < (int)sizeof(buf)) snprintf(buf + n, sizeof(buf) - n, " %.4s", qth);
        if (goes_out(buf)) {
            strcpy(out, buf);   /* "CQ_POTA" kept: "CQ POTA" encoded again at TX is a type 4 "CQ POTA", no call */
            return true;
        }
    }
    snprintf(buf, sizeof(buf), "CQ %s", callsign);
    if (goes_out(buf)) {
        strcpy(out, buf);
        return true;
    }
    out[0] = '\0';
    return false;
}
