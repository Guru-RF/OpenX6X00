/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the Zello app's Simplex repeater rules (src/voice_link.c, shared with SVXConnect, whose own run of them
 *  is tools/svx/svx_link_test.c): it keys only when the rules let it and only inside the amateur bands, unkeys at the
 *  time limit and the moment the operator keys, never sends to Zello while the radio transmits (no loop), and sends
 *  the CW ID when it is due; the squelch as the audio shows it; the ID's Morse timing. `make` in tools/zello.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "voice_link.h"

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

/* The app whose link this is: tools/svx/svx_link_test.c runs the same cases as SVXConnect's */
#ifndef LINK_NET
#define LINK_NET       "Zello"
#define LINK_TEST_NAME "zello_link_test"
#endif

static vlink_cfg_t cfg(void) {
    return (vlink_cfg_t){.to_rf = true, .to_net = true, .id_on = true, .id_period_ms = 60 * MIN_MS,
                         .limit_ms = 120000, .pause_ms = 3000, .net = LINK_NET};
}

/* A world the link runs in: the inputs, and what voice_net.c would do with the outputs (its thread keyed or not) */
typedef struct {
    vlink_t     l;
    vlink_cfg_t c;
    vlink_in_t  in;
    int              keys, ids_with_key, unkeys, ids_at_unkey, talk_starts, talk_stops;
    bool             on_air;        /* we keyed and the thread has not ended */
    bool             to_zello;      /* a message to Zello is on */
} world_t;

static void world_init(world_t *w) {
    memset(w, 0, sizeof(*w));
    vlink_init(&w->l);
    w->c            = cfg();
    w->in.now_ms    = 1000000;
    w->in.connected = true;
    w->in.can_key   = true;
    w->in.ham_band  = true;
    w->in.rx_ham_band = true;
}

static vlink_out_t step(world_t *w) {
    w->in.keyed            = w->on_air;
    vlink_out_t o     = vlink_step(&w->l, &w->c, &w->in);
    w->in.operator_key     = false;
    w->in.rx_ended         = false;

    if (o.key) {
        CHECK(!w->on_air);          /* never two transmissions */
        CHECK(!w->to_zello);        /* never a key while a message goes to Zello */
        CHECK(!w->in.radio_busy);   /* never over someone else */
        CHECK(!w->in.squelch_open); /* never over a signal on the frequency */
        CHECK(w->in.can_key && w->in.connected);
        CHECK(w->in.ham_band);      /* never outside the amateur bands */
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
        CHECK(w->in.rx_ham_band || w->in.rx_outside_ok);   /* never from outside the bands, unless MARS, CAP */
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
static void thread_ends(world_t *w, vlink_tx_result_t r) {
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

    w.in.net_msg = true;
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
    w.c.to_net = false;
    step(&w);
    CHECK(w.keys == 0);                 /* switched off */
    w.c.to_rf = true;
    w.c.to_net = true;

    w.in.squelch_open = true;
    step(&w);
    CHECK(w.keys == 0 && w.talk_starts == 1);   /* someone on the frequency: relayed to Zello, not keyed over */
    w.in.squelch_open = false;
    step(&w);
    CHECK(w.talk_stops == 1 && w.keys == 0);    /* the squelch closed: the message to Zello ends, nothing keys yet */
    step(&w);
    CHECK(w.keys == 1 && w.l.state == VLINK_RELAY);
    CHECK(w.ids_with_key == 0);

    /* The message ends: unkeyed with the ID, the first transmission's */
    advance(&w, 8000);
    step(&w);
    CHECK(w.keys == 1 && w.unkeys == 0);
    w.in.net_msg = false;
    vlink_out_t e = step(&w);
    CHECK(w.unkeys == 1 && w.ids_at_unkey == 1);
    CHECK(!e.now);                              /* over: what is queued of it plays out */
    step(&w);
    CHECK(w.l.state == VLINK_ENDING);      /* until the thread has ended */
    advance(&w, 4000);                          /* the ID's 4 s */
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    CHECK(w.l.state == VLINK_IDLE);

    /* The next message waits for the pause */
    w.in.net_msg = true;
    advance(&w, 1000);
    step(&w);
    CHECK(w.keys == 1);
    advance(&w, 1999);
    step(&w);
    CHECK(w.keys == 1);
    advance(&w, 1);
    step(&w);
    CHECK(w.keys == 2);
    w.in.net_msg = false;
    step(&w);
    CHECK(w.unkeys == 2 && w.ids_at_unkey == 1);    /* the ID not due yet */
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
}

static void test_time_limit(void) {
    world_t w;

    world_init(&w);
    w.in.net_msg = true;
    step(&w);
    CHECK(w.keys == 1);
    advance(&w, 119999);
    step(&w);
    CHECK(w.unkeys == 0);
    advance(&w, 1);
    vlink_out_t o = step(&w);
    CHECK(w.unkeys == 1 && o.why != NULL);
    CHECK(o.id && o.now);                       /* the rest dropped; the first transmission's ID before unkeying */
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    /* The same message goes on: not keyed again, also after the pause */
    for (int i = 0; i < 100; i++) {
        advance(&w, 1000);
        step(&w);
    }
    CHECK(w.keys == 1);
    /* It ends, a new one comes: keyed */
    w.in.net_msg = false;
    step(&w);
    w.in.net_msg = true;
    step(&w);
    CHECK(w.keys == 2);

    /* The limit is the setting's */
    world_init(&w);
    w.c.limit_ms = 30000;
    w.in.net_msg = true;
    step(&w);
    advance(&w, 30000);
    step(&w);
    CHECK(w.unkeys == 1);
}

static void test_operator_wins(void) {
    world_t w;

    world_init(&w);
    w.in.net_msg = true;
    step(&w);
    CHECK(w.keys == 1);
    advance(&w, 2000);
    w.in.operator_key = true;
    vlink_out_t o = step(&w);
    CHECK(o.unkey && o.now && !o.id);           /* off at once, no ID over the operator */
    w.in.radio_busy = true;                     /* the operator on the air */
    thread_ends(&w, VLINK_TX_DONE);
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
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);

    /* While the ID is being sent at the end of a message: the operator stops it too */
    world_init(&w);
    w.in.net_msg = true;
    step(&w);
    w.in.net_msg = false;
    o = step(&w);
    CHECK(o.unkey && o.id);
    w.in.operator_key = true;
    o = step(&w);
    CHECK(o.unkey && o.now && !o.id);
    thread_ends(&w, VLINK_TX_DONE);
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

    /* Our own transmission: nothing to Zello while keyed, nor VLINK_HOLD_MS after */
    world_init(&w);
    w.in.net_msg = true;
    step(&w);
    CHECK(w.keys == 1);
    w.in.squelch_open = true;                   /* the receiver hears our own TX audio */
    for (int i = 0; i < 10; i++) {
        advance(&w, 100);
        step(&w);
    }
    CHECK(w.talk_starts == 0);
    w.in.net_msg = false;
    step(&w);
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    advance(&w, VLINK_HOLD_MS - 1);
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
    vlink_out_t o = step(&w);
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
    CHECK(!o.talk_stop && w.l.state == VLINK_IDLE);
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
    w.in.net_msg = true;
    step(&w);
    w.in.net_msg = false;
    vlink_out_t o = step(&w);
    CHECK(o.unkey && o.id);
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    uint64_t id1 = w.in.now_ms;
    CHECK(w.l.id_ms == id1);

    /* Another 5 min later: no ID */
    advance(&w, 5 * MIN_MS);
    w.in.net_msg = true;
    step(&w);
    w.in.net_msg = false;
    o = step(&w);
    CHECK(o.unkey && !o.id);
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    CHECK(w.l.tx_since_id);

    /* Nothing more to relay: the ID alone once the period since the last one has gone, not before */
    advance(&w, 5 * MIN_MS - 1000);
    step(&w);
    CHECK(w.ids_with_key == 0);
    w.in.squelch_open = true;                   /* the frequency busy at that moment: it waits */
    w.c.to_net = false;
    advance(&w, 1000);
    step(&w);
    CHECK(w.ids_with_key == 0);
    w.in.squelch_open = false;
    advance(&w, 30000);
    step(&w);
    CHECK(w.ids_with_key == 1 && w.l.state == VLINK_ID);
    advance(&w, 4000);
    thread_ends(&w, VLINK_TX_DONE);
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
    w.in.net_msg = true;
    step(&w);
    w.in.net_msg = false;
    o = step(&w);
    CHECK(o.unkey && !o.id);
    thread_ends(&w, VLINK_TX_DONE);
    for (int i = 0; i < 200; i++) {
        advance(&w, MIN_MS);
        step(&w);
    }
    CHECK(w.ids_with_key == 0 && w.keys == 1);

    /* The default hour: a message 59 min after the first ID has none, one at 60 min has */
    world_init(&w);
    w.in.net_msg = true;
    step(&w);
    w.in.net_msg = false;
    step(&w);
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    advance(&w, 59 * MIN_MS);
    w.in.net_msg = true;
    step(&w);
    w.in.net_msg = false;
    o = step(&w);
    CHECK(!o.id);
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    advance(&w, MIN_MS);
    w.in.net_msg = true;
    step(&w);
    w.in.net_msg = false;
    o = step(&w);
    CHECK(o.id);
    CHECK(w.ids_with_key == 0);                 /* no ID alone in between: the message carried it */

    /* Zello to RF switched off after a transmission (voice_net.c then asks with a period of 0): the ID owed goes at once */
    world_init(&w);
    w.c.to_net = false;
    w.in.net_msg = true;
    step(&w);
    w.in.net_msg = false;
    step(&w);
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    advance(&w, MIN_MS);
    w.in.net_msg = true;
    step(&w);
    CHECK(w.keys == 2);
    w.c.to_rf = false;
    w.c.id_period_ms = 0;
    o = step(&w);
    CHECK(o.unkey && o.now && o.id);            /* off in the middle of a message: unkeyed with the ID */
    thread_ends(&w, VLINK_TX_DONE);
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
    w.in.net_msg = true;
    step(&w);
    thread_ends(&w, VLINK_TX_STOPPED);          /* SWR, a band stop, refused there */
    vlink_out_t o = step(&w);
    CHECK(o.why != NULL && w.l.to_rf_stopped);
    CHECK(o.why && !strcmp(o.why, LINK_NET " to RF stopped: the radio ended the transmission"));   /* the app's name */
    w.in.net_msg = false;
    step(&w);
    w.in.net_msg = true;
    for (int i = 0; i < 100; i++) {
        advance(&w, MIN_MS);
        step(&w);
    }
    CHECK(w.keys == 1);                         /* no relay, no ID: nothing keys */
    vlink_rearm(&w.l);
    step(&w);
    CHECK(w.keys == 2);

    /* Not keyed (the player taken meanwhile): nothing sent, tried again */
    world_init(&w);
    w.in.net_msg = true;
    step(&w);
    thread_ends(&w, VLINK_TX_NOT_KEYED);
    step(&w);
    CHECK(!w.l.to_rf_stopped && w.l.air_end_ms == 0);
    step(&w);
    CHECK(w.keys == 2);
}

/* Only inside the amateur bands or a transverter's range, whatever TX outside ham bands says: the CB and HF Air
 * Band apps tune outside them */
static void test_amateur_bands_only(void) {
    world_t     w;
    vlink_out_t o;

    world_init(&w);
    w.in.ham_band = false;
    w.in.net_msg  = true;
    o = step(&w);
    CHECK(w.keys == 0 && o.why && !strcmp(o.why, "Link: amateur bands only"));
    o = step(&w);
    CHECK(w.keys == 0 && o.why == NULL);            /* said once */
    for (int i = 0; i < 100; i++) {
        advance(&w, MIN_MS);
        step(&w);
    }
    CHECK(w.keys == 0);
    w.in.ham_band = true;                           /* back inside: the message goes */
    step(&w);
    CHECK(w.keys == 1 && w.l.state == VLINK_RELAY);

    /* Tuned out of the band while on the air: unkeyed at once, the rest of the message not sent, no ID there */
    w.in.ham_band = false;
    o = step(&w);
    CHECK(o.unkey && o.now && !o.id && o.why && strstr(o.why, "amateur bands only"));
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    CHECK(w.l.state == VLINK_IDLE && w.l.cut);
    w.in.ham_band = true;
    advance(&w, 10000);
    step(&w);
    CHECK(w.keys == 2 && w.ids_with_key == 1);      /* that message is not keyed again; the ID it owes goes alone */
    thread_ends(&w, VLINK_TX_DONE);
    w.in.net_msg = false;
    step(&w);
    CHECK(!w.l.tx_since_id && w.l.id_ms != 0);

    /* The ID owed is not sent outside them either, and goes once back inside */
    world_init(&w);
    w.in.net_msg = true;
    step(&w);
    w.in.net_msg = false;
    advance(&w, 5000);
    step(&w);                                       /* over, with the first ID */
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    w.in.net_msg = true;
    advance(&w, 5000);
    step(&w);                                       /* a second message, no ID */
    w.in.net_msg = false;
    advance(&w, 5000);
    step(&w);
    thread_ends(&w, VLINK_TX_DONE);
    step(&w);
    CHECK(w.keys == 2 && w.l.tx_since_id);
    w.in.ham_band = false;
    advance(&w, 61 * MIN_MS);
    o = step(&w);
    CHECK(w.keys == 2 && o.why && !strcmp(o.why, "Link: amateur bands only"));
    w.in.ham_band = true;
    o = step(&w);
    CHECK(w.keys == 3 && o.id);

    /* The receiver's frequency is what counts for RF to the network, the TX one for the key: in split (or with a TX
     * shift) out of the bands but listening inside them, the receiver still goes to the network */
    world_init(&w);
    w.in.ham_band     = false;
    w.in.squelch_open = true;
    step(&w);
    CHECK(w.talk_starts == 1 && w.keys == 0);
}

/* A language for the words: the format translated, the name put in after */
static const char *test_tr(const char *s) {
    return !strcmp(s, "Link: amateur bands only, nothing to %s") ? "Link: alleen amateurbanden, niets naar %s" : s;
}

/* RF to the network with TX outside ham bands off: nothing received outside the amateur bands (the CB, the airband,
 * broadcast) goes to a channel or a talkgroup */
static void test_rf_to_net_amateur_bands_only(void) {
    world_t     w;
    vlink_out_t o;

    world_init(&w);
    w.c.to_rf          = false;
    w.in.rx_ham_band   = false;
    w.in.squelch_open  = true;
    o = step(&w);
    CHECK(w.talk_starts == 0 && o.why && !strcmp(o.why, "Link: amateur bands only, nothing to " LINK_NET));
    o = step(&w);
    CHECK(w.talk_starts == 0 && o.why == NULL);     /* said once */
    for (int i = 0; i < 100; i++) {
        advance(&w, 1000);
        w.in.squelch_open = (i % 3) != 0;           /* signals come and go out there: still nothing, not said again */
        o = step(&w);
        CHECK(o.why == NULL);
    }
    CHECK(w.talk_starts == 0 && w.l.state == VLINK_IDLE);

    /* Back inside the bands with something on the frequency: it goes */
    w.in.rx_ham_band  = true;
    w.in.squelch_open = true;
    step(&w);
    CHECK(w.talk_starts == 1 && w.l.state == VLINK_TO_NET);

    /* Tuned out while it goes: ended at once, said; nothing more until back inside */
    advance(&w, 500);
    w.in.rx_ham_band = false;
    o = step(&w);
    CHECK(o.talk_stop && w.talk_stops == 1 && w.l.state == VLINK_IDLE);
    CHECK(o.why && !strcmp(o.why, "Link: amateur bands only, RF to " LINK_NET " ended"));
    for (int i = 0; i < 20; i++) {
        advance(&w, 500);
        o = step(&w);
        CHECK(o.why == NULL);
    }
    CHECK(w.talk_starts == 1);
    w.in.rx_ham_band = true;                        /* back: the station heard there goes, and leaving is said again */
    step(&w);
    CHECK(w.talk_starts == 2);
    w.in.rx_ham_band = false;
    o = step(&w);
    CHECK(o.talk_stop && o.why != NULL);

    /* Listening outside the bands while the TX frequency is inside (split): the key still works for the network's
     * messages, RF to the network does not */
    world_init(&w);
    w.in.rx_ham_band = false;
    w.in.net_msg     = true;
    step(&w);
    CHECK(w.keys == 1 && w.l.state == VLINK_RELAY);

    /* The frequencies themselves (vlink_rx_band): the amateur bands' edges in, the apps that tune outside them out,
     * a transverter's range in */
    CHECK(vlink_rx_band(7100000, false));
    CHECK(vlink_rx_band(14000000, false) && vlink_rx_band(14350000, false));
    CHECK(!vlink_rx_band(13999999, false) && !vlink_rx_band(14350001, false));
    CHECK(vlink_rx_band(28000000, false) && vlink_rx_band(50100000, false));
    CHECK(!vlink_rx_band(27185000, false));         /* CB channel 19 */
    CHECK(!vlink_rx_band(26965000, false));         /* CB channel 1 */
    CHECK(!vlink_rx_band(8891000, false));          /* HF air band (the North Atlantic) */
    CHECK(vlink_rx_band(5450000, false));           /* the 60 m band's top edge, still in */
    CHECK(!vlink_rx_band(5505000, false));          /* Shannon VOLMET */
    CHECK(!vlink_rx_band(6000000, false));          /* 49 m broadcast */
    CHECK(!vlink_rx_band(100000000, false));        /* FM broadcast */
    CHECK(!vlink_rx_band(127500000, false));        /* VHF airband */
    CHECK(!vlink_rx_band(145500000, false));        /* 2 m without a transverter: not a band of the radio's */
    CHECK(vlink_rx_band(145500000, true));          /* ... with one on that covers it */

    /* Said in the screen's language */
    world_init(&w);
    w.c.tr            = test_tr;
    w.c.to_rf         = false;
    w.in.rx_ham_band  = false;
    w.in.squelch_open = true;
    o = step(&w);
    CHECK(o.why && !strcmp(o.why, "Link: alleen amateurbanden, niets naar " LINK_NET));
}

/* With TX outside ham bands on (MARS, CAP) the receiver goes to the network outside the bands too, said once when
 * it starts out there and when the receiver tunes out while it is on, not at every message; the key stays inside */
static void test_rf_to_net_outside_with_tx_outside_on(void) {
    world_t     w;
    vlink_out_t o;
    const char *warn = "RF>" LINK_NET ": outside the amateur bands, check your licence";

    world_init(&w);
    w.c.to_rf          = false;
    w.in.rx_outside_ok = true;
    w.in.rx_ham_band   = false;
    o = step(&w);                                   /* RF to the network on out there: said as it starts */
    CHECK(o.why && !strcmp(o.why, warn));
    w.in.squelch_open = true;
    o = step(&w);
    CHECK(w.talk_starts == 1 && w.l.state == VLINK_TO_NET && o.why == NULL);
    w.in.squelch_open = false;
    for (int i = 0; i < 4; i++) {
        advance(&w, 500);
        o = step(&w);
        CHECK(o.why == NULL);
    }
    CHECK(w.talk_stops == 1);
    w.in.squelch_open = true;                       /* the next message: not said again */
    o = step(&w);
    CHECK(w.talk_starts == 2 && o.why == NULL);
    w.in.squelch_open = false;
    for (int i = 0; i < 4; i++) {
        advance(&w, 500);
        step(&w);
    }

    /* Back inside the bands: nothing to say; out again: said again */
    w.in.rx_ham_band = true;
    o = step(&w);
    CHECK(o.why == NULL);
    w.in.rx_ham_band = false;
    o = step(&w);
    CHECK(o.why && !strcmp(o.why, warn));

    /* Tuned out while a message goes: it goes on, said once */
    w.in.rx_ham_band = true;
    step(&w);
    w.in.squelch_open = true;
    step(&w);
    CHECK(w.talk_starts == 3 && w.l.state == VLINK_TO_NET);
    w.in.rx_ham_band = false;
    o = step(&w);
    CHECK(!o.talk_stop && w.l.state == VLINK_TO_NET && o.why && !strcmp(o.why, warn));
    advance(&w, 500);
    o = step(&w);
    CHECK(o.why == NULL && w.l.state == VLINK_TO_NET);

    /* TX outside switched off meanwhile: that message ends at once, the next is refused */
    w.in.rx_outside_ok = false;
    o = step(&w);
    CHECK(o.talk_stop && w.l.state == VLINK_IDLE && o.why && strstr(o.why, "amateur bands only"));
    advance(&w, 500);
    step(&w);
    CHECK(w.talk_starts == 3);

    /* On again, RF to the network switched off and on out there: said again as it starts */
    w.in.rx_outside_ok = true;
    w.in.squelch_open  = false;
    w.c.to_net         = false;
    o = step(&w);
    CHECK(o.why == NULL);
    w.c.to_net = true;
    o = step(&w);
    CHECK(o.why && !strcmp(o.why, warn));

    /* The key stays inside the amateur bands whatever TX outside them says: a message from the network out there is
     * not keyed */
    world_init(&w);
    w.in.rx_outside_ok = true;
    w.in.rx_ham_band   = false;
    w.in.ham_band      = false;
    w.in.net_msg       = true;
    for (int i = 0; i < 20; i++) {
        advance(&w, 1000);
        step(&w);
    }
    CHECK(w.keys == 0);

    /* Inside the bands it is as without it: nothing said */
    world_init(&w);
    w.in.rx_outside_ok = true;
    w.in.squelch_open  = true;
    o = step(&w);
    CHECK(w.talk_starts == 1 && o.why == NULL);
}

static void test_squelch(void) {
    vlink_sql_t s = {0};

    CHECK(!vlink_sql_step(&s, 2000, 20, 1500));
    CHECK(!vlink_sql_step(&s, 100, 20, 1500));          /* a click: not open */
    CHECK(!vlink_sql_step(&s, 2000, 20, 1500));
    CHECK(!vlink_sql_step(&s, 2000, 20, 1500));
    CHECK(vlink_sql_step(&s, 2000, 20, 1500));          /* 60 ms over */
    for (int i = 0; i < 74; i++) CHECK(vlink_sql_step(&s, 50, 20, 1500));
    CHECK(vlink_sql_step(&s, 400, 20, 1500));           /* between the levels: still open, the hang starts over */
    for (int i = 0; i < 74; i++) CHECK(vlink_sql_step(&s, 50, 20, 1500));
    CHECK(!vlink_sql_step(&s, 50, 20, 1500));           /* 1.5 s quiet: closed */
    CHECK(!vlink_sql_step(&s, 400, 20, 1500));
}

static void test_morse(void) {
    CHECK(vlink_morse_ms("E", 20) == 60);
    CHECK(vlink_morse_ms("T", 20) == 180);
    CHECK(vlink_morse_ms("PARIS", 20) == 43 * 60);
    CHECK(vlink_morse_ms("PARIS PARIS", 20) == (43 + 7 + 43) * 60);
    CHECK(vlink_morse_ms("paris", 20) == 43 * 60);
    CHECK(vlink_morse_ms("ON6URE/P", 20) == vlink_morse_ms("ON6URE", 20) + (3 + 13 + 3 + 11) * 60);   /* / and P */
    CHECK(vlink_morse_ms("", 20) == 0);

    static int16_t buf[44100 * 10];
    size_t         n = vlink_morse_render("ON6URE", 20, 800, 44100, 0.25f, buf, sizeof(buf) / sizeof(buf[0]));

    CHECK(n == (size_t)vlink_morse_ms("ON6URE", 20) * 44100 / 1000);
    int peak = 0;
    for (size_t i = 0; i < n; i++) peak = abs(buf[i]) > peak ? abs(buf[i]) : peak;
    CHECK(peak > 7000 && peak <= 8192);
    CHECK(buf[0] == 0);                                  /* a soft edge, no click */
    CHECK(vlink_morse_render("ON6URE", 20, 800, 44100, 0.25f, buf, 100) == 100);
}

int main(void) {
    test_keys_only_within_the_rules();
    test_time_limit();
    test_operator_wins();
    test_no_loop();
    test_id_timing();
    test_radio_stops_it();
    test_amateur_bands_only();
    test_rf_to_net_amateur_bands_only();
    test_rf_to_net_outside_with_tx_outside_on();
    test_squelch();
    test_morse();
    printf(LINK_TEST_NAME ": %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
