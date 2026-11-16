/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of what the SVXConnect app reads of its helper (src/svx_proto.c): the talkgroups with their priorities
 *  and flags, who talks on which, the recent overs, the selection and the lock, the transmission's events and the
 *  certificate; a line that does not read changes nothing it should not, an entry that does not read is left out,
 *  a callsign with ':' in it does not shift the numbers, and nothing is written past its field. `make` in tools/svx.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "svx_proto.h"

#include <stdio.h>
#include <string.h>

static int failed, checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            failed++;                                                                   \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                               \
    } while (0)

static char why[96];

static svx_ev_t line(svx_state_t *s, const char *l, uint64_t now) {
    return svx_proto_line(s, l, now, why, sizeof(why));
}

static void test_start(void) {
    svx_state_t s;

    svx_proto_init(&s);
    CHECK(s.conn == SVX_CONN_NONE && s.nodes == -1 && s.tg == 0 && !s.tx);
    /* What the helper says at the start (its lines as tools/svx/svx_helper_test.py saw them) */
    CHECK(line(&s, "hello 0.1.4", 0) == SVX_EV_NONE && !strcmp(s.version, "0.1.4"));
    line(&s, "conn connecting", 0);
    CHECK(s.conn == SVX_CONN_CONNECTING && s.why[0] == 0);
    line(&s, "tg 8 0", 0);
    CHECK(s.tg == 8 && !s.locked);
    line(&s, "tgs 8:2:s 1745:1:s 9990:0:-", 0);
    CHECK(s.n_tgs == 3);
    CHECK(s.tgs[0].id == 8 && s.tgs[0].prio == 2 && s.tgs[0].sw && !s.tgs[0].muted);
    CHECK(s.tgs[1].id == 1745 && s.tgs[1].prio == 1 && s.tgs[1].sw);
    CHECK(s.tgs[2].id == 9990 && s.tgs[2].prio == 0 && !s.tgs[2].sw && !s.tgs[2].muted);
    CHECK(svx_proto_tg_index(&s, 9990) == 2 && svx_proto_tg_index(&s, 1) == -1);
    CHECK(line(&s, "active", 0) == SVX_EV_NONE && s.n_active == 0);
    CHECK(line(&s, "recent", 0) == SVX_EV_NONE && s.n_recent == 0);
    CHECK(line(&s, "tx off", 0) == SVX_EV_NONE && !s.tx);      /* not on: no event */
    line(&s, "banner", 0);
    CHECK(s.banner[0] == 0);
    line(&s, "cert valid 1794844724 300", 0);
    CHECK(!strcmp(s.cert, "valid") && s.cert_end == 1794844724 && s.cert_days == 300);
    line(&s, "conn connected", 0);
    CHECK(s.conn == SVX_CONN_CONNECTED);
    line(&s, "server refl.example.org 5300 42 17", 0);
    CHECK(!strcmp(s.host, "refl.example.org") && s.port == 5300 && s.client_id == 42 && s.nodes == 17);
    line(&s, "tg 1745 1", 0);
    CHECK(s.tg == 1745 && s.locked);
    line(&s, "banner Welcome to the reflector", 0);
    CHECK(!strcmp(s.banner, "Welcome to the reflector"));
}

static void test_talkers(void) {
    svx_state_t s;

    svx_proto_init(&s);
    line(&s, "tg 8 0", 100000);
    line(&s, "active 1745:ON4XYZ:2500 8:ON7ABC:0", 100000);
    CHECK(s.n_active == 2);
    CHECK(s.active[0].tg == 1745 && !strcmp(s.active[0].call, "ON4XYZ") && s.active[0].since_ms == 97500);
    CHECK(svx_proto_talker(&s) == &s.active[1]);                /* the selected talkgroup's */
    CHECK(svx_proto_talker_on(&s, 1745) == &s.active[0] && svx_proto_talker_on(&s, 9990) == NULL);
    line(&s, "tg 0 0", 100000);
    CHECK(svx_proto_talker(&s) == NULL);                        /* monitoring only: nobody's audio is ours */
    line(&s, "active", 101000);
    CHECK(s.n_active == 0);

    /* A ':' in a callsign: the numbers are counted from the right */
    line(&s, "active 8:ON4:ODD:CALL:1200", 50000);
    CHECK(s.n_active == 1 && !strcmp(s.active[0].call, "ON4:ODD:CALL") && s.active[0].since_ms == 48800);
    line(&s, "recent 8:ON4:ODD:5:12 1745:ON4XYZ:60:3", 200000);
    CHECK(s.n_recent == 2 && !strcmp(s.recent[0].call, "ON4:ODD") && s.recent[0].end_ms == 195000);
    CHECK(s.recent[0].dur_s == 12 && s.recent[1].tg == 1745 && s.recent[1].end_ms == 140000 && s.recent[1].dur_s == 3);

    /* Entries that do not read are left out, the others kept */
    line(&s, "active x:ON4A:1 8::5 8:ON4B 8:ON4C:1x 9990:ON4D:7", 1000);
    CHECK(s.n_active == 1 && s.active[0].tg == 9990 && !strcmp(s.active[0].call, "ON4D"));
    line(&s, "recent 8:ON4A:1 8:ON4B:2:3", 10000);
    CHECK(s.n_recent == 1 && !strcmp(s.recent[0].call, "ON4B"));
    line(&s, "tgs 0:1:s 8:x:s 8:2 99999999999:0:s 8:4:sm 9990:12:s 1745:3:m", 0);
    CHECK(s.n_tgs == 2 && s.tgs[0].id == 8 && s.tgs[0].prio == 4 && s.tgs[0].sw && s.tgs[0].muted);
    CHECK(s.tgs[1].id == 1745 && s.tgs[1].prio == 3 && !s.tgs[1].sw && s.tgs[1].muted);

    /* A time from before the caller's clock started: 0, not a wrap */
    line(&s, "active 8:ON4A:5000", 1000);
    CHECK(s.n_active == 1 && s.active[0].since_ms == 0);
    line(&s, "recent 8:ON4A:5:1", 1000);
    CHECK(s.n_recent == 1 && s.recent[0].end_ms == 0);

    /* Lost: nobody talks any more */
    line(&s, "conn reconnecting the reflector closed the connection", 0);
    CHECK(s.conn == SVX_CONN_RECONNECTING && !strcmp(s.why, "the reflector closed the connection") && s.n_active == 0);
    line(&s, "conn idle quit", 0);
    CHECK(s.conn == SVX_CONN_IDLE && !strcmp(s.why, "quit"));
    line(&s, "conn sideways", 0);
    CHECK(s.conn == SVX_CONN_IDLE);                             /* a word it does not know changes nothing */
}

static void test_limits(void) {
    svx_state_t s;
    char        l[4096];
    size_t      n = 0;

    svx_proto_init(&s);
    n += (size_t)snprintf(l + n, sizeof(l) - n, "tgs");
    for (int i = 1; i <= 64; i++) n += (size_t)snprintf(l + n, sizeof(l) - n, " %d:0:s", i);
    line(&s, l, 0);
    CHECK(s.n_tgs == SVX_TGS_MAX && s.tgs[SVX_TGS_MAX - 1].id == SVX_TGS_MAX);

    n = (size_t)snprintf(l, sizeof(l), "active");
    for (int i = 1; i <= 40; i++) n += (size_t)snprintf(l + n, sizeof(l) - n, " %d:ON%dAA:0", i, i);
    line(&s, l, 0);
    CHECK(s.n_active == SVX_ACTIVE_MAX);

    n = (size_t)snprintf(l, sizeof(l), "recent");
    for (int i = 1; i <= 40; i++) n += (size_t)snprintf(l + n, sizeof(l) - n, " %d:ON%dAA:1:1", i, i);
    line(&s, l, 0);
    CHECK(s.n_recent == SVX_RECENT_MAX);

    /* A long callsign and long words stop at their fields, with their ends in place */
    line(&s, "active 8:ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:1", 10);
    CHECK(s.n_active == 1 && strlen(s.active[0].call) == SVX_CALL_LEN - 1);
    memset(l, 'x', 300);
    memcpy(l, "banner ", 7);
    l[300] = 0;
    line(&s, l, 0);
    CHECK(strlen(s.banner) == sizeof(s.banner) - 1);
    memcpy(l, "error  ", 7);
    CHECK(line(&s, l, 0) == SVX_EV_ERROR && strlen(s.error) == sizeof(s.error) - 1 && strlen(why) == sizeof(why) - 1);
    line(&s, "server a-very-long-host-name-that-goes-on-and-on-and-on-for-ever-and-ever.example.org 1 2 3", 0);
    CHECK(s.nodes == -1 || strlen(s.host) < sizeof(s.host));

    /* Control characters are not passed on to the screen */
    line(&s, "banner a\tb\x1b[31mc", 0);
    CHECK(!strchr(s.banner, '\t') && !strchr(s.banner, 0x1b));
}

static void test_tx(void) {
    svx_state_t s;

    svx_proto_init(&s);
    CHECK(line(&s, "tx on 8", 0) == SVX_EV_TX_ON && s.tx && s.tx_tg == 8);
    CHECK(line(&s, "tx off", 0) == SVX_EV_TX_OFF && !s.tx && why[0] == 0);
    CHECK(line(&s, "tx off", 0) == SVX_EV_NONE);                /* said again: nothing ended */
    line(&s, "tx on 1745", 0);
    CHECK(line(&s, "tx off TX timeout (120 s)", 0) == SVX_EV_TX_OFF && !strcmp(why, "TX timeout (120 s)"));
    CHECK(line(&s, "refused ON7ABC is talking on TG 8", 0) == SVX_EV_REFUSED && !strcmp(why, "ON7ABC is talking on TG 8"));
    CHECK(line(&s, "refused", 0) == SVX_EV_REFUSED && why[0] == 0);
    CHECK(line(&s, "error not enrolled: no certificate yet", 0) == SVX_EV_ERROR);
    CHECK(!strcmp(why, "not enrolled: no certificate yet") && !strcmp(s.error, why));
    CHECK(line(&s, "txt on 8", 0) == SVX_EV_NONE && !s.tx);      /* only the words themselves */
    CHECK(line(&s, "tx onward", 0) == SVX_EV_NONE && !s.tx);
    CHECK(line(&s, "", 0) == SVX_EV_NONE);
    CHECK(svx_proto_line(&s, "refused x", 0, NULL, 0) == SVX_EV_REFUSED);   /* no room for why: fine */
}

static void test_cert(void) {
    svx_state_t s;

    svx_proto_init(&s);
    line(&s, "cert renewal due 1794844724 12", 0);
    CHECK(!strcmp(s.cert, "renewal due") && s.cert_end == 1794844724 && s.cert_days == 12);
    line(&s, "cert not yet valid 1794844724 400", 0);
    CHECK(!strcmp(s.cert, "not yet valid") && s.cert_days == 400);
    line(&s, "cert expired 1700000000 -3", 0);
    CHECK(!strcmp(s.cert, "expired") && s.cert_days == -3);
    line(&s, "cert missing 0 0", 0);
    CHECK(!strcmp(s.cert, "missing") && s.cert_end == 0);
    line(&s, "cert garbage", 0);
    CHECK(!strcmp(s.cert, "missing"));                          /* unchanged */
    line(&s, "cert 5 6", 0);
    CHECK(!strcmp(s.cert, "missing"));
}

int main(void) {
    test_start();
    test_talkers();
    test_limits();
    test_tx();
    test_cert();
    printf("svx_proto_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
