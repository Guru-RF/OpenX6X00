/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the SVXConnect app's Simplex repeater: the same rules as Zello's (src/voice_link.c, one module for the
 *  two apps), so the same cases (tools/zello/zello_link_test.c) run here with SVXConnect's name in what the link says,
 *  and the ones its name changes are checked with it: keyed only within the rules and inside the amateur bands, the
 *  time limits, the operator first, no loop, the CW ID. `make` in tools/svx.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#define LINK_NET       "SVX"
#define LINK_TEST_NAME "svx_link_test"
#define main           zello_cases
#include "../zello/zello_link_test.c"
#undef main

/* What the link says carries the app's name, never the other app's */
static void test_svx_words(void) {
    world_t     w;
    vlink_out_t o;
    bool        limit_said = false, rx_limit_said = false;

    world_init(&w);
    w.in.net_msg = true;
    step(&w);
    CHECK(w.keys == 1);
    for (int i = 0; i < 130 && !limit_said; i++) {
        advance(&w, 1000);
        o = step(&w);
        if (o.why && !strcmp(o.why, "SVX to RF: time limit, unkeyed")) limit_said = true;
        CHECK(!o.why || !strstr(o.why, "Zello"));
    }
    CHECK(limit_said);

    world_init(&w);
    w.in.squelch_open = true;
    for (int i = 0; i < 400 && !rx_limit_said; i++) {
        advance(&w, 1000);
        o = step(&w);
        if (o.why && !strcmp(o.why, "RF to SVX: time limit, until the squelch closes")) rx_limit_said = true;
        CHECK(!o.why || !strstr(o.why, "Zello"));
    }
    CHECK(rx_limit_said && w.talk_starts == 1);
}

int main(void) {
    int rc = zello_cases();

    test_svx_words();
    printf(LINK_TEST_NAME ": SVX's words, %d checks in all, %d failed\n", checks, failed);
    return rc || failed ? 1 : 0;
}
