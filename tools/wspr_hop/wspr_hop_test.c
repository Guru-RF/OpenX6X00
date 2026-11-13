/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/wspr/wspr_hop.c (#190): the minute of the hour picks the band, Off bands give their slots to the
 *  other bands that are on, in turn, 6 m only in those, a band set to RX never carries a beacon, nor one set to RX+TX
 *  with the ATU on and no match stored, and the band changes only between two cycles.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "wspr/wspr_hop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            fails++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

enum { B160, B80, B60, B40, B30, B20, B17, B15, B12, B10, B6 };

/* 2026-11-13 00:00:00 UTC: a cycle that starts at the top of an hour */
#define DAY0 ((time_t)1794528000)

static int32_t only(const int *list, int n, wspr_use_t use) {
    int32_t bands = 0;

    for (int i = 0; i < n; i++) {
        bands = wspr_hop_set_use(bands, list[i], use);
    }
    return bands;
}

/* ---- The bands and the parameter ------------------------------------------------------- */

static void test_bands(void) {
    static const int     m[WSPR_HOP_BANDS]    = {160, 80, 60, 40, 30, 20, 17, 15, 12, 10, 6};
    /* sql/params.sql, type 4 */
    static const int32_t dial[WSPR_HOP_BANDS] = {1836600,  3568600,  5287200,  7038600,  10138700, 14095600,
                                                 18104600, 21094600, 24924600, 28124600, 50293000};

    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        CHECK(wspr_hop_band_m(i) == m[i], "band %d is %d m, not %d m", i, wspr_hop_band_m(i), m[i]);
        CHECK(wspr_hop_band_dial(i) == dial[i], "band %d dial %d", i, wspr_hop_band_dial(i));
        CHECK(wspr_hop_band_of(dial[i]) == i, "%d Hz on band %d, not %d", dial[i], wspr_hop_band_of(dial[i]), i);
    }
    CHECK(wspr_hop_band_m(-1) == 0 && wspr_hop_band_m(WSPR_HOP_BANDS) == 0, "no band outside the list");
    CHECK(wspr_hop_band_of(7074000) == B40, "FT8 on 40 m is 40 m");
    CHECK(wspr_hop_band_of(5364700) == B60, "the other 60 m WSPR frequency is 60 m");
    CHECK(wspr_hop_band_of(2500000) == -1, "2.5 MHz is no band");
    CHECK(wspr_hop_band_of(475700) == -1, "630 m is not in the list");
    CHECK(wspr_hop_band_of(40680000) == -1, "8 m is not in the list");
    CHECK(wspr_hop_band_of(144489000) == -1, "2 m is not in the list");
    CHECK(wspr_hop_band_of(0) == -1, "0 Hz is no band");

    /* The default: the ten slot bands receive, 6 m off, nothing transmits */
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        wspr_use_t want = i == B6 ? WSPR_USE_OFF : WSPR_USE_RX;
        CHECK(wspr_hop_use(WSPR_HOP_DEFAULT, i) == want, "default of band %d: %d", i, wspr_hop_use(WSPR_HOP_DEFAULT, i));
    }
    CHECK(WSPR_HOP_MAX == 0x3fffff, "max %x", WSPR_HOP_MAX);

    /* Each band on its own two bits */
    int32_t bands = 0;
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        bands = wspr_hop_set_use(bands, i, (wspr_use_t)(i % 3));
    }
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        CHECK(wspr_hop_use(bands, i) == (wspr_use_t)(i % 3), "band %d kept %d", i, wspr_hop_use(bands, i));
    }
    CHECK(bands >= 0 && bands <= WSPR_HOP_MAX, "in the parameter's range: %d", bands);
    bands = wspr_hop_set_use(bands, B40, WSPR_USE_OFF);
    CHECK(wspr_hop_use(bands, B40) == WSPR_USE_OFF && wspr_hop_use(bands, B60) == WSPR_USE_TX &&
              wspr_hop_use(bands, B30) == WSPR_USE_RX, "one band changed, its neighbours kept");
    CHECK(wspr_hop_set_use(bands, -1, WSPR_USE_TX) == bands && wspr_hop_set_use(bands, 11, WSPR_USE_TX) == bands,
          "a band outside the list changes nothing");
    CHECK(wspr_hop_use(3 << (2 * B20), B20) == WSPR_USE_OFF, "a pair of bits that is no use is Off");
    CHECK(wspr_hop_use(WSPR_HOP_MAX, -1) == WSPR_USE_OFF && wspr_hop_use(WSPR_HOP_MAX, 11) == WSPR_USE_OFF,
          "outside the list: Off");

    CHECK(wspr_hop_any(WSPR_HOP_DEFAULT, WSPR_USE_RX) && !wspr_hop_any(WSPR_HOP_DEFAULT, WSPR_USE_TX),
          "the default receives, transmits nowhere");
    CHECK(!wspr_hop_any(0, WSPR_USE_RX) && !wspr_hop_any(WSPR_HOP_MAX, WSPR_USE_RX), "all off");
    CHECK(wspr_hop_any(wspr_hop_set_use(0, B6, WSPR_USE_TX), WSPR_USE_TX) &&
              wspr_hop_any(wspr_hop_set_use(0, B6, WSPR_USE_TX), WSPR_USE_RX), "6 m RX+TX counts for both");
    CHECK(wspr_hop_next_use(WSPR_USE_OFF) == WSPR_USE_RX && wspr_hop_next_use(WSPR_USE_RX) == WSPR_USE_TX &&
              wspr_hop_next_use(WSPR_USE_TX) == WSPR_USE_OFF, "Off, RX, RX+TX, Off");
    CHECK(!strcmp(wspr_hop_use_name(WSPR_USE_OFF), "Off") && !strcmp(wspr_hop_use_name(WSPR_USE_RX), "RX") &&
              !strcmp(wspr_hop_use_name(WSPR_USE_TX), "RX+TX"), "the names");
}

/* ---- The minute of the hour picks the band -------------------------------------------- */

static void test_slots(void) {
    /* WSJT-X's table: 160 m at :00, 80 m :02 ... 10 m :18, again from :20 and :40 */
    for (int h = 0; h < 24; h++) {
        for (int min = 0; min < 60; min += 2) {
            time_t c    = DAY0 + h * 3600 + min * 60;
            int    want = (min % 20) / 2;

            CHECK(wspr_hop_slot(c) == want, "%02d:%02d: slot %d, not %d", h, min, wspr_hop_slot(c), want);
            CHECK(wspr_hop_pick(WSPR_HOP_DEFAULT, c) == want, "%02d:%02d default: band %d, not %d", h, min,
                  wspr_hop_pick(WSPR_HOP_DEFAULT, c), want);
            /* every band on, 6 m too: still the slot's band, 6 m has none */
            CHECK(wspr_hop_pick(WSPR_HOP_MAX & 0x2aaaaa, c) == want, "%02d:%02d all RX+TX", h, min);
        }
    }
    CHECK(wspr_hop_slot(DAY0 + 6 * 60) == B40 && wspr_hop_slot(DAY0 + 26 * 60) == B40 &&
              wspr_hop_slot(DAY0 + 46 * 60) == B40, "40 m at :06, :26 and :46");
    CHECK(wspr_hop_slot(DAY0 + 18 * 60) == B10 && wspr_hop_slot(DAY0 + 20 * 60) == B160, "10 m at :18, 160 m at :20");
    /* a time inside a cycle is its cycle's slot */
    CHECK(wspr_hop_slot(DAY0 + 10 * 60 + 119) == B20, "20 m to :11:59");
}

/* ---- Off bands, 6 m, all off ------------------------------------------------------------ */

static void test_off(void) {
    int counts[WSPR_HOP_BANDS];

    /* 60 m off (no licence there): its slot goes to the others, never to 60 m, and only its slot changes */
    int32_t bands = wspr_hop_set_use(WSPR_HOP_DEFAULT, B60, WSPR_USE_OFF);
    memset(counts, 0, sizeof(counts));
    for (int k = 0; k < 30 * 24; k++) {
        time_t c = DAY0 + k * 120;
        int    b = wspr_hop_pick(bands, c);

        CHECK(b >= 0 && b != B60 && b != B6, "cycle %d: band %d", k, b);
        if (wspr_hop_slot(c) != B60) {
            CHECK(b == wspr_hop_slot(c), "cycle %d: its own slot's band %d, not %d", k, wspr_hop_slot(c), b);
        } else if (b >= 0) {
            counts[b]++;
        }
    }
    /* 72 Off slots a day over 9 bands: 8 each */
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        int want = i == B60 || i == B6 ? 0 : 8;
        CHECK(counts[i] == want, "60 m's slots a day on band %d: %d, not %d", i, counts[i], want);
    }

    /* Only 40 m and 20 m: every cycle on one of them, 40 m at :06 and 20 m at :10, the rest in turn */
    static const int two[] = {B40, B20};
    bands = only(two, 2, WSPR_USE_TX);
    memset(counts, 0, sizeof(counts));
    int last = -1, same = 0;
    for (int k = 0; k < 30 * 24; k++) {
        time_t c    = DAY0 + k * 120;
        int    b    = wspr_hop_pick(bands, c);
        int    slot = wspr_hop_slot(c);

        CHECK(b == B40 || b == B20, "cycle %d: band %d", k, b);
        if (slot == B40 || slot == B20) {
            CHECK(b == slot, "cycle %d: the slot of %d m on %d m", k, wspr_hop_band_m(slot), wspr_hop_band_m(b));
        } else {
            same += b == last;      /* two Off slots in a row go to the two bands in turn */
            last = b;
            counts[b]++;
        }
    }
    CHECK(counts[B40] == counts[B20] && counts[B40] == 288, "Off slots shared: 40 m %d, 20 m %d", counts[B40], counts[B20]);
    CHECK(same == 0, "%d Off slots after one on the same band", same);

    /* 6 m on: it takes its turn in the Off slots, never in a slot whose band is on */
    static const int hf6[] = {B40, B30, B20, B6};
    bands = only(hf6, 4, WSPR_USE_RX);
    memset(counts, 0, sizeof(counts));
    for (int k = 0; k < 30 * 24; k++) {
        time_t c = DAY0 + k * 120;
        int    b = wspr_hop_pick(bands, c);

        if (b == B6) {
            int slot = wspr_hop_slot(c);
            CHECK(slot != B40 && slot != B30 && slot != B20, "6 m in the slot of %d m", wspr_hop_band_m(slot));
        }
        counts[b < 0 ? 0 : b]++;
    }
    /* a day: 72 slots each for 40, 30, 20 m and 504 Off slots over the four bands */
    CHECK(counts[B6] == 126, "6 m %d cycles a day", counts[B6]);
    CHECK(counts[B40] == 72 + 126 && counts[B30] == 72 + 126 && counts[B20] == 72 + 126, "40 m %d, 30 m %d, 20 m %d",
          counts[B40], counts[B30], counts[B20]);

    /* Only 6 m: every cycle there */
    static const int six[] = {B6};
    bands = only(six, 1, WSPR_USE_TX);
    for (int k = 0; k < 30; k++) {
        CHECK(wspr_hop_pick(bands, DAY0 + k * 120) == B6, "only 6 m, cycle %d", k);
    }

    /* Every band off: no band (the app stays where it is) */
    CHECK(wspr_hop_pick(0, DAY0) == -1, "all off");
    CHECK(wspr_hop_pick(WSPR_HOP_MAX, DAY0) == -1, "all on 'no use' bits: all off");

    /* The same pick on every radio and every call: no random */
    bands = wspr_hop_set_use(WSPR_HOP_DEFAULT, B160, WSPR_USE_OFF);
    CHECK(wspr_hop_pick(bands, DAY0 + 3600) == wspr_hop_pick(bands, DAY0 + 3600), "the same pick twice");
}

/* ---- TX: RX+TX bands only, and the ATU rule ------------------------------------------- */

static void test_tx(void) {
    int32_t bands = wspr_hop_set_use(WSPR_HOP_DEFAULT, B40, WSPR_USE_TX);

    CHECK(wspr_hop_tx_ok(bands, B40, false, false), "40 m RX+TX, ATU off: a beacon");
    CHECK(wspr_hop_tx_ok(bands, B40, true, true), "40 m RX+TX, ATU on with a match: a beacon");
    CHECK(!wspr_hop_tx_ok(bands, B40, true, false), "40 m RX+TX, ATU on, no match stored: RX only");
    for (int atu = 0; atu < 2; atu++) {
        for (int match = 0; match < 2; match++) {
            CHECK(!wspr_hop_tx_ok(bands, B20, atu, match), "20 m RX: never a beacon (ATU %d match %d)", atu, match);
            CHECK(!wspr_hop_tx_ok(bands, B6, atu, match), "6 m Off: never a beacon");
            CHECK(!wspr_hop_tx_ok(bands, -1, atu, match), "no band: never a beacon");
        }
    }
    CHECK(!wspr_hop_tx_ok(WSPR_HOP_DEFAULT, B40, false, false), "the default transmits nowhere");
}

/* ---- Never in the middle of a cycle ----------------------------------------------------- */

/* The cycle thread polls every 100 ms; the first time in a cycle the hop may be asked, -1 never */
static double first_hop(bool tx_cycle, bool late_decode) {
    for (int k = 0; k < 1200; k++) {
        double t         = k * 0.1;
        bool   on_air    = tx_cycle && t >= 1.0 && t < 1.0 + 110.6;
        bool   recording = !tx_cycle && t < (late_decode ? 115.0 : 114.0);

        if (wspr_hop_window(t, on_air, recording)) {
            return t;
        }
    }
    return -1;
}

static void test_window(void) {
    for (int k = 0; k < 1200; k++) {
        double t = k * 0.1;

        CHECK(!wspr_hop_window(t, true, false), "never while a beacon is on the air (%.1f s)", t);
        CHECK(!wspr_hop_window(t, false, true), "never while a cycle is recorded (%.1f s)", t);
        if (t < WSPR_HOP_FROM_S - 1e-9 || t >= WSPR_HOP_TO_S) {
            CHECK(!wspr_hop_window(t, false, false), "not at %.1f s into a cycle", t);
        }
    }
    double t = first_hop(false, false);
    CHECK(t >= 114.5 - 1e-9 && t < 114.6, "after an RX cycle at %.1f s", t);
    t = first_hop(true, false);
    CHECK(t >= 114.5 - 1e-9 && t < 114.6, "after a beacon at %.1f s", t);
    t = first_hop(false, true);
    CHECK(t >= 115.0 - 1e-9 && t < 115.1, "a late decode start first: %.1f s", t);
    CHECK(WSPR_HOP_FROM_S > 114.0 && WSPR_HOP_FROM_S > 1.0 + 110.6, "after the decode's samples and the beacon");
    CHECK(WSPR_HOP_TO_S <= 116.0, "before the next beacon is made");
}

/* ---- The frequencies the hop goes to ---------------------------------------------------- */

static void test_freqs(void) {
    int32_t list[16] = {1836600,  3568600,  5287200,  7038600,  10138700, 14095600, 18104600,
                        21094600, 24924600, 28124600, 50293000};
    int32_t f[WSPR_HOP_BANDS];

    wspr_hop_freqs(list, 11, f);
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        CHECK(f[i] == wspr_hop_band_dial(i), "band %d at %d", i, f[i]);
    }
    CHECK(wspr_hop_usable(WSPR_HOP_MAX & 0x2aaaaa, f) == (WSPR_HOP_MAX & 0x2aaaaa), "every band usable");

    /* A second 60 m frequency of the user's own: WSJT-X's stays the hop's; 630 m added: no band */
    list[11] = 5364700;
    list[12] = 475700;
    wspr_hop_freqs(list, 13, f);
    CHECK(f[B60] == 5287200, "60 m at %d", f[B60]);

    /* WSJT-X's 60 m deleted, the user's own in its place; 30 m deleted: none, so the hop cannot go there */
    list[2] = 5364700;
    list[4] = 475700;
    wspr_hop_freqs(list, 11, f);
    CHECK(f[B60] == 5364700, "60 m at the user's own %d", f[B60]);
    CHECK(f[B30] == 0, "30 m has none: %d", f[B30]);
    int32_t bands = wspr_hop_usable(WSPR_HOP_MAX & 0x2aaaaa, f);
    CHECK(wspr_hop_use(bands, B30) == WSPR_USE_OFF && wspr_hop_use(bands, B40) == WSPR_USE_TX, "30 m off, 40 m kept");
    for (int k = 0; k < 30; k++) {
        CHECK(wspr_hop_pick(bands, DAY0 + k * 120) != B30, "never to 30 m (cycle %d)", k);
    }
    wspr_hop_freqs(list, 0, f);
    CHECK(wspr_hop_pick(wspr_hop_usable(WSPR_HOP_DEFAULT, f), DAY0) == -1, "an empty list: no band");
}

int main(void) {
    test_bands();
    test_slots();
    test_off();
    test_tx();
    test_window();
    test_freqs();
    printf("wspr_hop: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
