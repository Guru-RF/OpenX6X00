/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - what the SVXConnect app's helper says (svxconnect --stdio, tools/svxconnect; its src/hostio.h has the
 *  whole exchange), read into the state the window shows, apart from the GUI so a PC can test it (tools/svx). Each
 *  line is sent when what it says changes, and all of them at the start:
 *
 *    hello <version>                    error <text>          (could not start: the program ends)
 *    conn connected | connecting | idle <why> | reconnecting <why>
 *    server <host> <port> <client id> <nodes>
 *    tg <selected> <locked 0|1>         tgs <id>:<priority>:<flags> ...   (flags s switchable, m muted, - neither)
 *    active <tg>:<call>:<ms> ...        recent <tg>:<call>:<ago s>:<s> ...
 *    tx on <tg> | tx off [<why>]        refused <why>
 *    cert <status> <not after> <days>   banner [<text>]
 *
 *  To it: the control commands of svxconnect's FIFO, one a line: tg <n>, lock on|off, ptt on|off, mute <n>, ...
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

#define SVX_TGS_MAX     32
#define SVX_ACTIVE_MAX  16
#define SVX_RECENT_MAX  20
#define SVX_CALL_LEN    24

typedef enum {
    SVX_CONN_NONE,              /* nothing heard yet */
    SVX_CONN_CONNECTING,
    SVX_CONN_CONNECTED,
    SVX_CONN_RECONNECTING,      /* lost, trying again: why */
    SVX_CONN_IDLE,              /* not connected: why */
} svx_conn_t;

typedef struct {
    uint32_t id;
    int      prio;              /* 0-3: how strongly its talkers pull the selection over */
    bool     sw;                /* switchable: it can be selected (and talked on) */
    bool     muted;
} svx_tg_t;

typedef struct {
    uint32_t tg;
    char     call[SVX_CALL_LEN];
    uint64_t since_ms;          /* when the over began, on the caller's clock */
} svx_talker_t;

typedef struct {
    uint32_t tg;
    char     call[SVX_CALL_LEN];
    uint64_t end_ms;            /* when the over ended, on the caller's clock */
    uint32_t dur_s;
} svx_recent_t;

typedef struct {
    char         version[16];
    svx_conn_t   conn;
    char         why[96];       /* the reason with idle and reconnecting */
    char         error[96];     /* "error": it could not start */
    char         host[64];
    unsigned     port, client_id;
    int          nodes;         /* -1 not known */
    uint32_t     tg;            /* selected, 0 monitoring only */
    bool         locked;
    svx_tg_t     tgs[SVX_TGS_MAX];
    int          n_tgs;
    svx_talker_t active[SVX_ACTIVE_MAX];
    int          n_active;
    svx_recent_t recent[SVX_RECENT_MAX];
    int          n_recent;
    bool         tx;            /* our own transmission on */
    uint32_t     tx_tg;
    char         cert[24];      /* valid, renewal due, expiring, expired, missing, ...; "" not heard */
    int64_t      cert_end;      /* its end, seconds since 1970; 0 none */
    int          cert_days;
    char         banner[96];
} svx_state_t;

typedef enum {
    SVX_EV_NONE,
    SVX_EV_TX_ON,               /* our transmission started */
    SVX_EV_TX_OFF,              /* ... ended, why (may be "") */
    SVX_EV_REFUSED,             /* a ptt on that did not start, why */
    SVX_EV_ERROR,               /* it could not start, why */
} svx_ev_t;

void svx_proto_init(svx_state_t *s);
/* One line (without its newline) at now_ms; what it means for a transmission, with why into why (why_n bytes) */
svx_ev_t svx_proto_line(svx_state_t *s, const char *line, uint64_t now_ms, char *why, size_t why_n);
/* Someone talking on the selected talkgroup, NULL nobody: whose audio plays */
const svx_talker_t *svx_proto_talker(const svx_state_t *s);
/* Who talks on tg now, NULL nobody */
const svx_talker_t *svx_proto_talker_on(const svx_state_t *s, uint32_t tg);
/* Its index in tgs, -1 none */
int svx_proto_tg_index(const svx_state_t *s, uint32_t tg);

#ifdef __cplusplus
}
#endif
