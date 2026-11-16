/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the Zello app's Simplex repeater rules (src/zello_link.c): it keys only when the rules let it, unkeys
 *  at the time limit and the moment the operator keys, never sends to Zello while the radio transmits (no loop), and
 *  sends the CW ID when it is due; the squelch as the audio shows it; the ID's Morse timing. `make` in tools/zello.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "zello_link.h"

#include <stdio.h>
#include <stdlib.h>
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

#define MIN_MS (60u * 1000u)

static zello_link_cfg_t cfg(void) {
    return (zello_link_cfg_t){.to_rf = true, .to_zello = true, .id_on = true, .id_period_ms = 60 * MIN_MS,
                              .limit_ms = 120000, .pause_ms = 3000};
}

/* A world the link runs in: the inputs, and what zello.c would do with the outputs (its thread keyed or not) */
typedef struct {
    zello_link_t     l;
    zello_link_cfg_t c;
    zello_link_in_t  in;
    int              keys, ids_with_key, unkeys, ids_at_unkey, talk_starts, talk_stops;
    bool             on_air;        /* we keyed and the thread has not ended */
    bool             to_zello;      /* a message to Zello is on */
} world_t;

static void world_init(world_t *w) {
    memset(w, 0, sizeof(*w));
    zello_link_init(&w->l);
    w->c            = cfg();
    w->in.now_ms    = 1000000;
    w->in.connected = true;
    w->in.can_key   = true;
}

static zello_link_out_t step(world_t *w) {
    w->in.keyed            = w->on_air;
    zello_link_out_t o     = zello_link_step(&w->l, &w->c, &w->in);
    w->in.operator_key     = false;
    w->in.rx_ended         = false;

    if (o.key) {
        CHECK(!w->on_air);          /* never two transmissions */
        CHECK(!w->to_zello);        /* never a key while a message goes to Zello */
        CHECK(!w->in.radio_busy);   /* never over someone else */
        CHECK(!w->in.squelch_open); /* never over a signal on the frequency */
        CHECK(w->in.can_key && w->in.connected);
        w->keys++;
        w->ids_with_key += o.id;
        w->on_air = true;
    }
    if (o.unkey) {
        w->unkeys++;
        w->ids_at_unkey += o.id;
    }
    if (o.talk_start) {
        CHECK(!w->on_air && !w->in.radio_busy);     /* no loop: never while the radio transmits */
        w->talk_starts++;
        w->to_zello = true;
    }
    if (o.talk_stop) {
        w->talk_stops++;
        w->to_zello = false;
    }
    return o;
}

/* The thread ends: how */
static void thread_ends(world_t *w, zello_tx_result_t r) {
    w->on_air    = false;
    w->in.result = r;
}

static void advance(world_t *w, uint32_t ms) {
    w->in.now_ms += ms;
}

static void test_keys_only_within_the_rules(void) {
    world_t w;

    world_init(&w);
    step(&w);
    CHECK(w.keys == 0);                 /* nothing to send */

    w.in.zello_msg = true;
    w.in.connected = false;
    step(&w);
    CHECK(w.keys == 0);                 /* off the channel */
    w.in.connected = true;

    w.in.radio_busy = true;
    step(&w);
    CHECK(w.keys == 0);                 /* the operator, an app, CAT on the air */
    w.in.radio_busy = false;

    w.in.can_key = false;
    step(&w);
    CHECK(w.keys == 0);                 /* not a voice mode, or no callsign for the ID */
    w.in.can_key = true;

    w.c.to_rf = false;
    w.c.to_zello = false;
    step(&w);
    CHECK(w.keys == 0);                 /* switched off */
    w.c.to_rf = true;
    w.c.to_zello = true;

    w.in.squelch_open = true;
    step(&w);
    CHECK(w.keys == 0 && w.talk_starts == 1);   /* someone on the frequency: relayed to Zello, not keyed over */
    w.in.squelch_open = false;
    step(&w);
    CHECK(w.talk_stops == 1 && w.keys == 0);    /* the squelch closed: the message to Zello ends, nothing keys yet */
    step(&w);
    CHECK(w.keys == 1 && w.l.state == ZELLO_LINK_RELAY);
    CHECK(w.ids_with_key == 0);

    /* The message ends: unkeyed with the ID, the first transmission's */
    advance(&w, 8000);
    step(&w);
    CHECK(w.keys == 1 && w.unkeys == 0);
    w.in.zello_msg = false;
    zello_link_out_t e = step(&w);
    CHECK(w.unkeys == 1 && w.ids_at_unkey == 1);
    CHECK(!e.now);                              /* over: what is queued of it plays out */
    step(&w);
    CHECK(w.l.state == ZELLO_LINK_ENDING);      /* until the thread has ended */
    advance(&w, 4000);                          /* the ID's 4 s */
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    CHECK(w.l.state == ZELLO_LINK_IDLE);

    /* The next message waits for the pause */
    w.in.zello_msg = true;
    advance(&w, 1000);
    step(&w);
    CHECK(w.keys == 1);
    advance(&w, 1999);
    step(&w);
    CHECK(w.keys == 1);
    advance(&w, 1);
    step(&w);
    CHECK(w.keys == 2);
    w.in.zello_msg = false;
    step(&w);
    CHECK(w.unkeys == 2 && w.ids_at_unkey == 1);    /* the ID not due yet */
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
}

static void test_time_limit(void) {
    world_t w;

    world_init(&w);
    w.in.zello_msg = true;
    step(&w);
    CHECK(w.keys == 1);
    advance(&w, 119999);
    step(&w);
    CHECK(w.unkeys == 0);
    advance(&w, 1);
    zello_link_out_t o = step(&w);
    CHECK(w.unkeys == 1 && o.why != NULL);
    CHECK(o.id && o.now);                       /* the rest dropped; the first transmission's ID before unkeying */
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    /* The same message goes on: not keyed again, also after the pause */
    for (int i = 0; i < 100; i++) {
        advance(&w, 1000);
        step(&w);
    }
    CHECK(w.keys == 1);
    /* It ends, a new one comes: keyed */
    w.in.zello_msg = false;
    step(&w);
    w.in.zello_msg = true;
    step(&w);
    CHECK(w.keys == 2);

    /* The limit is the setting's */
    world_init(&w);
    w.c.limit_ms = 30000;
    w.in.zello_msg = true;
    step(&w);
    advance(&w, 30000);
    step(&w);
    CHECK(w.unkeys == 1);
}

static void test_operator_wins(void) {
    world_t w;

    world_init(&w);
    w.in.zello_msg = true;
    step(&w);
    CHECK(w.keys == 1);
    advance(&w, 2000);
    w.in.operator_key = true;
    zello_link_out_t o = step(&w);
    CHECK(o.unkey && o.now && !o.id);           /* off at once, no ID over the operator */
    w.in.radio_busy = true;                     /* the operator on the air */
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    for (int i = 0; i < 50; i++) {
        advance(&w, 1000);
        step(&w);
    }
    CHECK(w.keys == 1);                         /* not while the operator transmits */
    w.in.radio_busy = false;
    advance(&w, 10000);
    step(&w);
    /* The rest of that message is not sent; the ID owed (transmitted, none yet) goes alone once the frequency is free */
    CHECK(w.keys == 2 && w.ids_with_key == 1);
    advance(&w, 4000);
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);

    /* While the ID is being sent at the end of a message: the operator stops it too */
    world_init(&w);
    w.in.zello_msg = true;
    step(&w);
    w.in.zello_msg = false;
    o = step(&w);
    CHECK(o.unkey && o.id);
    w.in.operator_key = true;
    o = step(&w);
    CHECK(o.unkey && o.now && !o.id);
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    CHECK(w.l.tx_since_id && w.l.id_ms == 0);   /* the ID did not go: still owed */
}

static void test_no_loop(void) {
    world_t w;

    world_init(&w);
    w.c.to_rf = false;
    w.in.squelch_open = true;
    step(&w);
    CHECK(w.talk_starts == 1);
    /* The radio transmits (the operator, an app): the message to Zello ends at once, none starts meanwhile */
    w.in.radio_busy = true;
    step(&w);
    CHECK(w.talk_stops == 1);
    for (int i = 0; i < 20; i++) {
        advance(&w, 100);
        step(&w);
    }
    CHECK(w.talk_starts == 1);
    w.in.radio_busy = false;
    step(&w);
    CHECK(w.talk_starts == 2);
    w.in.squelch_open = false;
    step(&w);
    CHECK(w.talk_stops == 2);

    /* Our own transmission: nothing to Zello while keyed, nor ZELLO_LINK_HOLD_MS after */
    world_init(&w);
    w.in.zello_msg = true;
    step(&w);
    CHECK(w.keys == 1);
    w.in.squelch_open = true;                   /* the receiver hears our own TX audio */
    for (int i = 0; i < 10; i++) {
        advance(&w, 100);
        step(&w);
    }
    CHECK(w.talk_starts == 0);
    w.in.zello_msg = false;
    step(&w);
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    advance(&w, ZELLO_LINK_HOLD_MS - 1);
    step(&w);
    CHECK(w.talk_starts == 0);
    advance(&w, 1);
    step(&w);
    CHECK(w.talk_starts == 1);                  /* a station on the frequency after our TX: to Zello */

    /* A message to Zello cut at the limit: not again until the squelch has closed */
    world_init(&w);
    w.c.to_rf = false;
    w.in.squelch_open = true;
    step(&w);
    advance(&w, 120000);
    zello_link_out_t o = step(&w);
    CHECK(o.talk_stop && o.why != NULL);
    for (int i = 0; i < 10; i++) {
        advance(&w, 1000);
        step(&w);
    }
    CHECK(w.talk_starts == 1);
    w.in.squelch_open = false;
    step(&w);
    w.in.squelch_open = true;
    step(&w);
    CHECK(w.talk_starts == 2);
    /* Refused by the helper (a listen-only channel): not again until the squelch closes */
    w.in.rx_ended = true;
    o = step(&w);
    CHECK(!o.talk_stop && w.l.state == ZELLO_LINK_IDLE);
    w.to_zello = false;
    advance(&w, 1000);
    step(&w);
    CHECK(w.talk_starts == 2);
}

static void test_id_timing(void) {
    world_t w;

    world_init(&w);
    w.c.id_period_ms = 10 * MIN_MS;
    /* One message: its ID at the end */
    w.in.zello_msg = true;
    step(&w);
    w.in.zello_msg = false;
    zello_link_out_t o = step(&w);
    CHECK(o.unkey && o.id);
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    uint64_t id1 = w.in.now_ms;
    CHECK(w.l.id_ms == id1);

    /* Another 5 min later: no ID */
    advance(&w, 5 * MIN_MS);
    w.in.zello_msg = true;
    step(&w);
    w.in.zello_msg = false;
    o = step(&w);
    CHECK(o.unkey && !o.id);
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    CHECK(w.l.tx_since_id);

    /* Nothing more to relay: the ID alone once the period since the last one has gone, not before */
    advance(&w, 5 * MIN_MS - 1000);
    step(&w);
    CHECK(w.ids_with_key == 0);
    w.in.squelch_open = true;                   /* the frequency busy at that moment: it waits */
    w.c.to_zello = false;
    advance(&w, 1000);
    step(&w);
    CHECK(w.ids_with_key == 0);
    w.in.squelch_open = false;
    advance(&w, 30000);
    step(&w);
    CHECK(w.ids_with_key == 1 && w.l.state == ZELLO_LINK_ID);
    advance(&w, 4000);
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    CHECK(!w.l.tx_since_id);
    /* Quiet after: no more IDs */
    for (int i = 0; i < 30; i++) {
        advance(&w, MIN_MS);
        step(&w);
    }
    CHECK(w.ids_with_key == 1 && w.keys == 3);

    /* The ID off: never */
    world_init(&w);
    w.c.id_on = false;
    w.in.zello_msg = true;
    step(&w);
    w.in.zello_msg = false;
    o = step(&w);
    CHECK(o.unkey && !o.id);
    thread_ends(&w, ZELLO_TX_DONE);
    for (int i = 0; i < 200; i++) {
        advance(&w, MIN_MS);
        step(&w);
    }
    CHECK(w.ids_with_key == 0 && w.keys == 1);

    /* The default hour: a message 59 min after the first ID has none, one at 60 min has */
    world_init(&w);
    w.in.zello_msg = true;
    step(&w);
    w.in.zello_msg = false;
    step(&w);
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    advance(&w, 59 * MIN_MS);
    w.in.zello_msg = true;
    step(&w);
    w.in.zello_msg = false;
    o = step(&w);
    CHECK(!o.id);
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    advance(&w, MIN_MS);
    w.in.zello_msg = true;
    step(&w);
    w.in.zello_msg = false;
    o = step(&w);
    CHECK(o.id);
    CHECK(w.ids_with_key == 0);                 /* no ID alone in between: the message carried it */

    /* Zello to RF switched off after a transmission (zello.c then asks with a period of 0): the ID owed goes at once */
    world_init(&w);
    w.c.to_zello = false;
    w.in.zello_msg = true;
    step(&w);
    w.in.zello_msg = false;
    step(&w);
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    advance(&w, MIN_MS);
    w.in.zello_msg = true;
    step(&w);
    CHECK(w.keys == 2);
    w.c.to_rf = false;
    w.c.id_period_ms = 0;
    o = step(&w);
    CHECK(o.unkey && o.now && o.id);            /* off in the middle of a message: unkeyed with the ID */
    thread_ends(&w, ZELLO_TX_DONE);
    step(&w);
    for (int i = 0; i < 100; i++) {
        advance(&w, MIN_MS);
        step(&w);
    }
    CHECK(w.keys == 2 && !w.l.tx_since_id);     /* and nothing after */
}

static void test_radio_stops_it(void) {
    world_t w;

    world_init(&w);
    w.in.zello_msg = true;
    step(&w);
    thread_ends(&w, ZELLO_TX_STOPPED);          /* SWR, a band stop, refused there */
    zello_link_out_t o = step(&w);
    CHECK(o.why != NULL && w.l.to_rf_stopped);
    w.in.zello_msg = false;
    step(&w);
    w.in.zello_msg = true;
    for (int i = 0; i < 100; i++) {
        advance(&w, MIN_MS);
        step(&w);
    }
    CHECK(w.keys == 1);                         /* no relay, no ID: nothing keys */
    zello_link_rearm(&w.l);
    step(&w);
    CHECK(w.keys == 2);

    /* Not keyed (the player taken meanwhile): nothing sent, tried again */
    world_init(&w);
    w.in.zello_msg = true;
    step(&w);
    thread_ends(&w, ZELLO_TX_NOT_KEYED);
    step(&w);
    CHECK(!w.l.to_rf_stopped && w.l.air_end_ms == 0);
    step(&w);
    CHECK(w.keys == 2);
}

static void test_squelch(void) {
    zello_sql_t s = {0};

    CHECK(!zello_sql_step(&s, 2000, 20, 1500));
    CHECK(!zello_sql_step(&s, 100, 20, 1500));          /* a click: not open */
    CHECK(!zello_sql_step(&s, 2000, 20, 1500));
    CHECK(!zello_sql_step(&s, 2000, 20, 1500));
    CHECK(zello_sql_step(&s, 2000, 20, 1500));          /* 60 ms over */
    for (int i = 0; i < 74; i++) CHECK(zello_sql_step(&s, 50, 20, 1500));
    CHECK(zello_sql_step(&s, 400, 20, 1500));           /* between the levels: still open, the hang starts over */
    for (int i = 0; i < 74; i++) CHECK(zello_sql_step(&s, 50, 20, 1500));
    CHECK(!zello_sql_step(&s, 50, 20, 1500));           /* 1.5 s quiet: closed */
    CHECK(!zello_sql_step(&s, 400, 20, 1500));
}

static void test_morse(void) {
    CHECK(zello_morse_ms("E", 20) == 60);
    CHECK(zello_morse_ms("T", 20) == 180);
    CHECK(zello_morse_ms("PARIS", 20) == 43 * 60);
    CHECK(zello_morse_ms("PARIS PARIS", 20) == (43 + 7 + 43) * 60);
    CHECK(zello_morse_ms("paris", 20) == 43 * 60);
    CHECK(zello_morse_ms("ON6URE/P", 20) == zello_morse_ms("ON6URE", 20) + (3 + 13 + 3 + 11) * 60);   /* / and P */
    CHECK(zello_morse_ms("", 20) == 0);

    static int16_t buf[44100 * 10];
    size_t         n = zello_morse_render("ON6URE", 20, 800, 44100, 0.25f, buf, sizeof(buf) / sizeof(buf[0]));

    CHECK(n == (size_t)zello_morse_ms("ON6URE", 20) * 44100 / 1000);
    int peak = 0;
    for (size_t i = 0; i < n; i++) peak = abs(buf[i]) > peak ? abs(buf[i]) : peak;
    CHECK(peak > 7000 && peak <= 8192);
    CHECK(buf[0] == 0);                                  /* a soft edge, no click */
    CHECK(zello_morse_render("ON6URE", 20, 800, 44100, 0.25f, buf, 100) == 100);
}

int main(void) {
    test_keys_only_within_the_rules();
    test_time_limit();
    test_operator_wins();
    test_no_loop();
    test_id_timing();
    test_radio_stops_it();
    test_squelch();
    test_morse();
    printf("zello_link_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
