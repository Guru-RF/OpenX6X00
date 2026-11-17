/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  For dmr_test.py: the helper's text lines on stdin through src/dmr_proto.c, the app's state after the last as
 *  key=value lines on stdout, so the test checks what the app would show of the helper's real lines.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dmr_proto.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    static const char *conn[] = {"off", "connecting", "online", "retry", "error"};
    dmr_state_t        s;
    char               line[512], who[64];
    int                calls = 0, ends = 0;

    dmr_proto_init(&s);
    while (fgets(line, sizeof(line), stdin)) {
        dmr_ev_t ev = dmr_proto_line(&s, line, 0);

        calls += ev == DMR_EV_CALL;
        ends += ev == DMR_EV_END;
    }
    printf("conn=%s\nwhy=%s\nmaster=%s\nhotspot=%u\ntg=%u\ntalking=%d\ncalls=%d\nends=%d\n", conn[s.conn], s.why,
           s.master, (unsigned)s.hotspot, (unsigned)s.tg, s.talking, calls, ends);
    for (int k = 0; k < s.n_heard; k++)
        printf("heard=%u %s %u %u\n", (unsigned)s.heard[k].id, dmr_proto_who(&s.heard[k], who, sizeof(who)),
               (unsigned)s.heard[k].dst, (unsigned)s.heard[k].secs);
    return 0;
}
