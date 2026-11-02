/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/base_guard.c (#152): when "No data from the base" is said (base_gone_due), run through the flow
 *  resets as radio.c calls it, one every 0.3 s while no packet comes. Not in the first 30 s after a start, 30 s after
 *  the last packet, 30 s after the GUI's own TX however long the silence before it, never while the GUI keys, once,
 *  and again only after a packet came and the data stopped again.
 */

#include "base_guard.h"

#include <stdio.h>

#define RESET_MS 300        /* radio.c's FLOW_RESTART_TIMEOUT: a flow reset this often without a packet */
#define T0       1000000    /* the GUI's start, ms of the monotonic clock */
#define NONE     0

static int fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* The rule's state in radio.c: the last packet, the GUI's last TX, told */
typedef struct {
    uint64_t pack;
    uint64_t tx;
    bool     said;
} run_t;

/* The flow resets from `from` to `to` (ms after T0), the GUI keying from kf to kt (NONE: not): when it was said
 * first, in ms after T0, or NONE; said then, as base_gone does */
static long resets(run_t *r, long from, long to, long kf, long kt) {
    long first = NONE;

    for (long t = from; t <= to; t += RESET_MS) {
        bool keyed = kt != NONE && t >= kf && t <= kt;

        if (base_gone_due(T0 + t, r->pack, keyed, &r->tx, r->said)) {
            if (first == NONE) {
                first = t;
            }
            r->said = true;
        }
    }
    return first;
}

/* A packet at t: what radio_tick does */
static void packet(run_t *r, long t) {
    r->pack = T0 + t;
    r->said = false;
}

int main(void) {
    run_t r = { .pack = T0 };   /* radio_init: pack_time = the start */
    long  t;

    /* No data from the start: said at the first reset 30 s in, once */
    check(!base_gone_due(T0 + 29999, r.pack, false, &r.tx, false), "29.999 s after the start: not yet", 29999);
    check(base_gone_due(T0 + 30000, r.pack, false, &r.tx, false), "30 s after the start: due", 30000);
    t = resets(&r, 300, 120000, NONE, NONE);
    check(t >= 30000 && t < 30000 + RESET_MS, "no data from the start: said 30 s in", t);
    check(resets(&r, 120300, 300000, NONE, NONE) == NONE, "said once only, however long it lasts", 0);

    /* The data back, then gone again: said again 30 s after the last packet */
    packet(&r, 300000);
    check(!r.said, "a packet: told no more", 0);
    t = resets(&r, 300300, 400000, NONE, NONE);
    check(t >= 330000 && t < 330000 + RESET_MS, "lost again: said 30 s after the last packet", t);

    /* The first packet 16 s after the start (#47's 1.0.8 radio): nothing said */
    r = (run_t){ .pack = T0 };
    check(resets(&r, 300, 15900, NONE, NONE) == NONE, "a base slow to start: nothing in its first 16 s", 0);
    packet(&r, 16000);
    t = resets(&r, 16300, 100000, NONE, NONE);
    check(t >= 46000 && t < 46000 + RESET_MS, "then lost: 30 s after its last packet", t);

    /* The GUI keys: never while it does, then 30 s after it let go, however long the silence before */
    r = (run_t){ .pack = T0 };
    check(resets(&r, 300, 25000, 10000, 25000) == NONE, "keyed 10-25 s: nothing", 0);
    t = resets(&r, 25300, 100000, NONE, NONE);
    check(t >= 55000 && t < 55000 + RESET_MS, "let go at 25 s: said 30 s after", t);

    r = (run_t){ .pack = T0 };
    check(resets(&r, 300, 200000, 20000, 200000) == NONE, "keyed through a long silence: never", 0);
    t = resets(&r, 200300, 300000, NONE, NONE);
    check(t >= 230000 && t < 230000 + RESET_MS, "let go after it: 30 s after", t);

    /* A packet after the GUI's TX counts from the packet, a TX after a packet from the TX */
    r = (run_t){ .pack = T0 };
    resets(&r, 300, 5000, 3000, 5000);
    packet(&r, 10000);
    t = resets(&r, 10300, 100000, NONE, NONE);
    check(t >= 40000 && t < 40000 + RESET_MS, "TX at 5 s, a packet at 10 s: from the packet", t);

    r = (run_t){ .pack = T0 };
    packet(&r, 10000);
    resets(&r, 10300, 20000, 15000, 20000);
    t = resets(&r, 20300, 100000, NONE, NONE);
    check(t >= 50000 && t < 50000 + RESET_MS, "a packet at 10 s, TX to 20 s: from the TX", t);

    /* Said already: keying and letting go does not say it again without a packet */
    r = (run_t){ .pack = T0 };
    resets(&r, 300, 40000, NONE, NONE);
    check(r.said, "said at 30 s", 0);
    check(resets(&r, 40300, 200000, 50000, 60000) == NONE, "a TX after it, no packet: not again", 0);

    printf("base_guard: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
