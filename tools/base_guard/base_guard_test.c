/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/base_guard.c (#152): when "No data from the base" is said (base_gone_due), run through the flow
 *  resets as radio.c calls it, one every 0.3 s while no packet comes. 60 s after a start with no packet yet, 30 s
 *  after the last packet, 30 s after the GUI's own TX however long the silence before it, never while the GUI keys,
 *  once, and again only after a packet came and the data stopped again. Then the base log's one line a second
 *  (base_guard_log) and base_guard_log_free, which the flow resets' lines wait for, on the test's own clock.
 */

#include "base_guard.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static time_t now_s = 1800000000;

time_t __wrap_time(time_t *t) {
    if (t) {
        *t = now_s;
    }
    return now_s;
}

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

/* The rule's state in radio.c: the last packet, a packet came, the GUI's last TX, told */
typedef struct {
    uint64_t pack;
    bool     seen;
    uint64_t tx;
    bool     said;
} run_t;

/* The flow resets from `from` to `to` (ms after T0), the GUI keying from kf to kt (NONE: not): when it was said
 * first, in ms after T0, or NONE; said then, as base_gone does */
static long resets(run_t *r, long from, long to, long kf, long kt) {
    long first = NONE;

    for (long t = from; t <= to; t += RESET_MS) {
        bool keyed = kt != NONE && t >= kf && t <= kt;

        if (base_gone_due(T0 + t, r->pack, r->seen, keyed, &r->tx, r->said)) {
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
    r->seen = true;
    r->said = false;
}

int main(void) {
    run_t r = { .pack = T0 };   /* radio_init: pack_time = the start */
    long  t;

    /* No data from the start: said at the first reset 60 s in, once */
    check(!base_gone_due(T0 + 59999, r.pack, false, false, &r.tx, false), "59.999 s after the start: not yet", 59999);
    check(base_gone_due(T0 + 60000, r.pack, false, false, &r.tx, false), "60 s after the start: due", 60000);
    t = resets(&r, 300, 120000, NONE, NONE);
    check(t >= 60000 && t < 60000 + RESET_MS, "no data from the start: said 60 s in", t);
    check(resets(&r, 120300, 300000, NONE, NONE) == NONE, "said once only, however long it lasts", 0);

    /* The data back, then gone again: said again 30 s after the last packet */
    packet(&r, 300000);
    check(!r.said, "a packet: told no more", 0);
    t = resets(&r, 300300, 400000, NONE, NONE);
    check(t >= 330000 && t < 330000 + RESET_MS, "lost again: said 30 s after the last packet", t);

    /* The first packet 16 s after the start (#47's 1.0.8 radio), or 45 s: nothing said */
    r = (run_t){ .pack = T0 };
    check(resets(&r, 300, 15900, NONE, NONE) == NONE, "a base slow to start: nothing in its first 16 s", 0);
    packet(&r, 16000);
    t = resets(&r, 16300, 100000, NONE, NONE);
    check(t >= 46000 && t < 46000 + RESET_MS, "then lost: 30 s after its last packet", t);

    r = (run_t){ .pack = T0 };
    check(resets(&r, 300, 44900, NONE, NONE) == NONE, "a slower start: nothing in its first 45 s", 0);
    packet(&r, 45000);
    check(resets(&r, 45300, 74700, NONE, NONE) == NONE, "then lost: nothing in the next 30 s", 0);

    /* The GUI keys: never while it does, then 30 s after it let go, however long the silence before; with no packet
     * yet not before the start's 60 s */
    r = (run_t){ .pack = T0 };
    check(resets(&r, 300, 25000, 10000, 25000) == NONE, "keyed 10-25 s: nothing", 0);
    t = resets(&r, 25300, 100000, NONE, NONE);
    check(t >= 60000 && t < 60000 + RESET_MS, "let go at 25 s, no packet yet: the start's 60 s", t);

    r = (run_t){ .pack = T0 };
    check(resets(&r, 300, 50000, 40000, 50000) == NONE, "keyed 40-50 s: nothing", 0);
    t = resets(&r, 50300, 150000, NONE, NONE);
    check(t >= 80000 && t < 80000 + RESET_MS, "let go at 50 s: said 30 s after", t);

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
    resets(&r, 300, 70000, NONE, NONE);
    check(r.said, "said at 60 s", 0);
    check(resets(&r, 70300, 200000, 80000, 90000) == NONE, "a TX after it, no packet: not again", 0);

    /* The log: one line a second, the one more dropped and counted on the next; free when this second has none */
    char  buf[1024] = "";
    FILE *f;

    unlink("base_guard_test.log");
    check(base_guard_log_free(), "nothing written yet: free", 0);
    base_guard_log("first %d", 1);
    check(!base_guard_log_free(), "a line in this second: not free", 0);
    base_guard_log("dropped");
    now_s++;
    check(base_guard_log_free(), "the next second: free", 0);
    base_guard_log("second");
    check(!base_guard_log_free(), "and taken", 0);
    if ((f = fopen("base_guard_test.log", "r"))) {
        buf[fread(buf, 1, sizeof(buf) - 1, f)] = 0;
        fclose(f);
    }
    check(strstr(buf, "UTC first 1\n") != NULL, "the first line written", 0);
    check(strstr(buf, "dropped") == NULL, "the one in the same second dropped", 0);
    check(strstr(buf, "UTC second (1 more in the same second)\n") != NULL, "counted on the next line", 0);
    unlink("base_guard_test.log");

    printf("base_guard: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
