/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  For tools/svx/svx_helper_test.py: the helper's lines on stdin through src/svx_proto.c, as the app reads them; after
 *  each one line of what it made of it: the event, why, and the state (the connection, the selection and the lock, the
 *  talkgroups, the talkers, the recent overs, the transmission, the certificate). The clock: milliseconds from the
 *  number before each line ("<ms> <line>").
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "svx_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *EV[] = {"none", "tx_on", "tx_off", "refused", "error"};

int main(void) {
    svx_state_t s;
    char        buf[8192], why[96];

    svx_proto_init(&s);
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(buf, sizeof(buf), stdin)) {
        char    *end, *l;
        uint64_t now = strtoull(buf, &end, 10);

        buf[strcspn(buf, "\n")] = 0;
        l                       = *end == ' ' ? end + 1 : end;
        svx_ev_t ev             = svx_proto_line(&s, l, now, why, sizeof(why));
        const svx_talker_t *k   = svx_proto_talker(&s);

        printf("ev=%s|why=%s|conn=%d|tg=%u|locked=%d|nodes=%d|host=%s|tx=%d|tx_tg=%u|cert=%s|talker=%s|tgs=",
               EV[ev], why, (int)s.conn, (unsigned)s.tg, s.locked, s.nodes, s.host, s.tx, (unsigned)s.tx_tg, s.cert,
               k ? k->call : "");
        for (int i = 0; i < s.n_tgs; i++)
            printf("%s%u:%d:%s%s", i ? "," : "", (unsigned)s.tgs[i].id, s.tgs[i].prio, s.tgs[i].sw ? "s" : "",
                   s.tgs[i].muted ? "m" : "");
        printf("|active=");
        for (int i = 0; i < s.n_active; i++) printf("%s%u:%s", i ? "," : "", (unsigned)s.active[i].tg, s.active[i].call);
        printf("|recent=");
        for (int i = 0; i < s.n_recent; i++)
            printf("%s%u:%s:%u", i ? "," : "", (unsigned)s.recent[i].tg, s.recent[i].call, (unsigned)s.recent[i].dur_s);
        printf("\n");
    }
    return 0;
}
