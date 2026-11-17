/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - what the DMR app reads of its helper, rootfs/usr/sbin/x6100-dmr (dmr_proto.c): the text lines of its
 *  socket (the header of x6100-dmr has them) as the link's state, who talks now and the last heard. No LVGL, no
 *  threads: dialog_dmr.c keeps one under its lock, tools/dmr tests it alone and with the helper's own lines.
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

#define DMR_HEARD_MAX 12
#define DMR_NAME_MAX  48

typedef enum {
    DMR_CONN_OFF,           /* no helper */
    DMR_CONN_CONNECTING,    /* logging in */
    DMR_CONN_ONLINE,        /* listening */
    DMR_CONN_RETRY,         /* lost or not reached: again in retry_s s */
    DMR_CONN_ERROR,         /* given up: why */
} dmr_conn_t;

typedef struct {
    uint32_t id;                    /* the station's DMR ID */
    uint32_t dst;                   /* the talkgroup, or the user's DMR ID for a private call */
    char     name[DMR_NAME_MAX];    /* radioid.net's callsign and first name, "" not known (yet) */
    bool     private_call;
    uint8_t  slot;                  /* 1 or 2 */
    uint64_t start_ms;              /* the caller's clock when it started */
    uint32_t secs;                  /* a call heard: how long */
} dmr_call_t;

typedef struct {
    dmr_conn_t conn;
    char       why[112];            /* retry's or error's reason, as the helper says it */
    int        retry_s;
    char       master[64];
    uint32_t   hotspot;
    uint32_t   tg;                  /* asked for in the options, 0 none */
    bool       talking;
    dmr_call_t talker;              /* while talking */
    dmr_call_t heard[DMR_HEARD_MAX];    /* the newest first, a station once */
    int        n_heard;
} dmr_state_t;

typedef enum {
    DMR_EV_NONE,
    DMR_EV_ONLINE,
    DMR_EV_CALL,                    /* a call started */
    DMR_EV_END,                     /* it ended */
    DMR_EV_LOST,                    /* retry: a call on is over */
    DMR_EV_ERROR,                   /* the helper gave up */
} dmr_ev_t;

void     dmr_proto_init(dmr_state_t *s);
/* A line of the helper's (no newline), now_ms the caller's clock: what it changed */
dmr_ev_t dmr_proto_line(dmr_state_t *s, const char *line, uint64_t now_ms);
/* The call's name, or its DMR ID as digits when no name is known: into buf */
const char *dmr_proto_who(const dmr_call_t *c, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
