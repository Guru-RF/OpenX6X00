/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Host test of the JS8 board's red card (src/js8/js8_logic.h): a station that heard us (a heartbeat answered, a
 *  call) is red for JS8_HEARD_FRESH_MS after their last answer, 3 minutes, then a plain card with "heard you" in grey;
 *  a new answer makes it red again; one that never heard us is never red. From OlivLoze's fork (F4OLI, 14b215ad).
 */

#include "js8/js8_logic.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

int main(void) {
    const int64_t t0 = 1763550000000LL;     /* 19 Nov 2026, wall clock ms */
    js8_station_t st;

    memset(&st, 0, sizeof(st));
    snprintf(st.call, sizeof(st.call), "F4OLI");
    st.heard_ms = t0;
    check(JS8_HEARD_FRESH_MS == 180000, "3 minutes");
    check(!js8_heard_me_fresh(&st, t0), "one that never heard us: not red");

    st.heard_me    = true;
    st.heard_me_ms = t0;
    check(js8_heard_me_fresh(&st, t0), "red as they answer");
    check(js8_heard_me_fresh(&st, t0 + 179999), "red 2:59.999 after");
    check(!js8_heard_me_fresh(&st, t0 + 180000), "plain at 3:00");
    check(!js8_heard_me_fresh(&st, t0 + 3600000), "plain an hour after, still listed");

    st.heard_me_ms = t0 + 3600000;     /* they answer again */
    check(js8_heard_me_fresh(&st, t0 + 3600000 + 60000), "red again a minute after their next answer");

    st.heard_me = false;     /* never red without heard_me, whatever time is kept */
    check(!js8_heard_me_fresh(&st, t0 + 3600000), "heard_me off: not red");

    printf("js8 board: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
