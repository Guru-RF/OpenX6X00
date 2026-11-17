/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/dmr_proto.c, what the DMR app reads of its helper's lines (rootfs/usr/sbin/x6100-dmr): the link's
 *  states, a call from its start to its end with the name found later, the last heard (newest first, a station once,
 *  the oldest dropped when full), a lost link ending the call, the error, and lines that are broken or unknown
 *  changing nothing. Built with ASan and UBSan.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dmr_proto.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            fails++;                                                   \
        }                                                              \
    } while (0)

static void test_states(void) {
    dmr_state_t s;

    dmr_proto_init(&s);
    CHECK(s.conn == DMR_CONN_OFF);
    CHECK(dmr_proto_line(&s, "state connecting", 0) == DMR_EV_NONE && s.conn == DMR_CONN_CONNECTING);
    CHECK(dmr_proto_line(&s, "state online 127.0.0.1 999000101 2062", 0) == DMR_EV_ONLINE);
    CHECK(s.conn == DMR_CONN_ONLINE && !strcmp(s.master, "127.0.0.1") && s.hotspot == 999000101 && s.tg == 2062);
    CHECK(dmr_proto_line(&s, "state retry 20 the master stopped answering", 0) == DMR_EV_NONE);
    CHECK(s.conn == DMR_CONN_RETRY && s.retry_s == 20 && !strcmp(s.why, "the master stopped answering"));
    CHECK(dmr_proto_line(&s, "state connecting", 0) == DMR_EV_NONE && s.why[0] == 0);
    CHECK(dmr_proto_line(&s, "error the master refused the hotspot password", 0) == DMR_EV_ERROR);
    CHECK(s.conn == DMR_CONN_ERROR && !strcmp(s.why, "the master refused the hotspot password"));
}

static void test_call(void) {
    dmr_state_t s;
    char        who[64];

    dmr_proto_init(&s);
    dmr_proto_line(&s, "state online m.example 999000101 91", 0);
    CHECK(dmr_proto_line(&s, "call 9990001 91 group 2", 1000) == DMR_EV_CALL);
    CHECK(s.talking && s.talker.id == 9990001 && s.talker.dst == 91 && s.talker.slot == 2 && !s.talker.private_call);
    CHECK(s.talker.start_ms == 1000 && s.talker.name[0] == 0);
    CHECK(!strcmp(dmr_proto_who(&s.talker, who, sizeof(who)), "9990001"));
    dmr_proto_line(&s, "name 9990001 ON4ABC Jan", 0);
    CHECK(!strcmp(s.talker.name, "ON4ABC Jan"));
    CHECK(!strcmp(dmr_proto_who(&s.talker, who, sizeof(who)), "ON4ABC Jan"));
    CHECK(dmr_proto_line(&s, "end 9999999 3.0", 0) == DMR_EV_NONE && s.talking);    /* not this call's */
    CHECK(dmr_proto_line(&s, "end 9990001 12.4", 0) == DMR_EV_END && !s.talking);
    CHECK(s.n_heard == 1 && s.heard[0].id == 9990001 && s.heard[0].secs == 12 && !strcmp(s.heard[0].name, "ON4ABC Jan"));

    /* A private call to this hotspot, with its name at once */
    CHECK(dmr_proto_line(&s, "call 9990002 9990001 private 1 K1ABC Al", 0) == DMR_EV_CALL);
    CHECK(s.talker.private_call && s.talker.slot == 1 && !strcmp(s.talker.name, "K1ABC Al"));
    dmr_proto_line(&s, "end 9990002 0.6", 0);
    CHECK(s.n_heard == 2 && s.heard[0].id == 9990002 && s.heard[0].secs == 1 && s.heard[1].id == 9990001);

    /* The same station again: on top, once */
    dmr_proto_line(&s, "call 9990001 91 group 2", 0);
    dmr_proto_line(&s, "end 9990001 2", 0);
    CHECK(s.n_heard == 2 && s.heard[0].id == 9990001 && s.heard[0].secs == 2 && s.heard[1].id == 9990002);
    /* a name that comes later reaches the heard list too */
    dmr_proto_line(&s, "name 9990002 K1ABC Albert", 0);
    CHECK(!strcmp(s.heard[1].name, "K1ABC Albert"));

    /* The link lost during a call: the call is over, not heard */
    dmr_proto_line(&s, "call 2345678 91 group 2", 0);
    CHECK(dmr_proto_line(&s, "state retry 10 the master closed the link", 0) == DMR_EV_LOST && !s.talking);
    CHECK(s.n_heard == 2);
}

static void test_full(void) {
    dmr_state_t s;
    char        line[64];

    dmr_proto_init(&s);
    for (int i = 0; i < DMR_HEARD_MAX + 5; i++) {
        snprintf(line, sizeof(line), "call %d 91 group 2", 1000000 + i);
        dmr_proto_line(&s, line, 0);
        snprintf(line, sizeof(line), "end %d 1", 1000000 + i);
        dmr_proto_line(&s, line, 0);
    }
    CHECK(s.n_heard == DMR_HEARD_MAX);
    CHECK(s.heard[0].id == 1000000 + DMR_HEARD_MAX + 4);
    CHECK(s.heard[DMR_HEARD_MAX - 1].id == 1000000 + 5);
}

static void test_broken(void) {
    dmr_state_t s, before;
    const char *bad[] = {"", "call", "call x 91 group 2", "call 1 91 group 3", "call 1 91 group", "call 1 2",
                         "call 99999999999 91 group 2", "end", "end x 2", "name", "name x y", "state", "state what",
                         "hello world", "state online", "call -1 91 group 2",
                         "call 1 91 group 2 a-name-that-is-much-too-long-for-the-field-of-forty-eight-bytes-and-more"};

    dmr_proto_init(&s);
    dmr_proto_line(&s, "state online m 1 2", 0);
    for (size_t i = 0; i + 1 < sizeof(bad) / sizeof(bad[0]); i++) {
        before = s;
        dmr_proto_line(&s, bad[i], 0);
        if (i == 14) continue;      /* "state online" with nothing after it: online, with no master */
        CHECK(!memcmp(&before, &s, sizeof(s)));
    }
    /* A long name is cut, not overrun */
    CHECK(dmr_proto_line(&s, bad[sizeof(bad) / sizeof(bad[0]) - 1], 0) == DMR_EV_CALL);
    CHECK(strlen(s.talker.name) == DMR_NAME_MAX - 1);
    /* A line longer than the reader's buffer */
    char huge[1000];

    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    memcpy(huge, "error ", 6);
    CHECK(dmr_proto_line(&s, huge, 0) == DMR_EV_ERROR && strlen(s.why) < sizeof(s.why));
}

int main(void) {
    test_states();
    test_call();
    test_full();
    test_broken();
    printf("dmr_proto_test: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
