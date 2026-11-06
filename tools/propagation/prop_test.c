/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The sun (src/sun.c), the beacon schedule and its scoring (src/beacons.c) and the scanner's
 *  engine (src/scan.c), without the GUI (tools/propagation: make)
 */

#include "beacons.h"
#include "scan.h"
#include "sun.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fails++;                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                  \
            printf("\n");                         \
        }                                         \
    } while (0)

static double noise(double sd) {
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = rand() / (double)RAND_MAX;

    return sd * sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

static void test_sun(void) {
    double lat, lon;

    sun_subsolar(1718916660, &lat, &lon);   /* 2024-06-20 20:51 UTC, the June solstice */
    CHECK(fabs(lat - 23.44) < 0.1, "solstice lat %.2f", lat);
    sun_subsolar(1710903960, &lat, &lon);   /* 2024-03-20 03:06 UTC, the March equinox */
    CHECK(fabs(lat) < 0.1, "equinox lat %.2f", lat);
    sun_subsolar(1713182400, &lat, &lon);   /* 2024-04-15 12:00 UTC: the equation of time is about 0 */
    CHECK(fabs(lon) < 1.0, "noon lon %.2f", lon);
    sun_subsolar(1713182400 + 6 * 3600, &lat, &lon);   /* six hours later, a quarter of the way west */
    CHECK(fabs(lon + 90) < 1.0, "18 UTC lon %.2f", lon);

    /* Greenwich, 2024-06-20 at 12:01:40 UTC (its solar noon): 90 - 51.48 + 23.43 */
    sun_subsolar(1718884900, &lat, &lon);
    double e = sun_elevation(lat, lon, 51.48, 0.0);
    CHECK(fabs(e - 61.95) < 0.6, "noon elevation %.2f", e);
    CHECK(sun_light(e) == SUN_DAY, "day");
    sun_subsolar(1718884900 + 12 * 3600, &lat, &lon);
    CHECK(sun_light(sun_elevation(lat, lon, 51.48, 0.0)) == SUN_NIGHT, "midnight");
}

static void test_schedule(void) {
    time_t hour = 1718913600;   /* 2024-06-20 20:00 UTC */

    CHECK(beacon_on(0, hour) == 0, "4U1UN opens the hour on 20 m");
    CHECK(beacon_on(0, hour + 10) == 1, "VE8AT next on 20 m");
    CHECK(beacon_on(1, hour + 10) == 0, "4U1UN on 17 m 10 s in");
    CHECK(beacon_on(4, hour + 40) == 0, "4U1UN on 10 m 40 s in");
    CHECK(beacon_on(0, hour + 179) == 17, "YV5B last on 20 m");
    CHECK(beacon_on(0, hour + 180) == 0, "the round again");
    CHECK(beacon_band_of(0, hour + 40) == 4, "band of 4U1UN at 40 s");
    CHECK(beacon_band_of(0, hour + 50) == -1, "4U1UN off at 50 s");
    CHECK(beacon_band_of(13, hour + 130 + 9) == 0, "OH2B on 20 m in slot 13");
    CHECK(beacon_band_at(14100000) == 0 && beacon_band_at(28200300) == 4 && beacon_band_at(7040000) == -1, "bands");
    for (int b = 0; b < BEACON_BANDS; b++)       /* every slot: each band one beacon, all different */
        for (int s = 0; s < BEACONS_N; s++)
            CHECK(beacon_band_of(beacon_on(b, hour + 10 * s), hour + 10 * s) == b, "band %d slot %d", b, s);
    CHECK(!strcmp(beacon_power_heard(8), "100 W") && !strcmp(beacon_power_heard(40), "0.1 W"), "power");
}

/* 20 m for two rounds: a noise of -110 dB, OH2B 22 dB over it and VK6RBP 4 dB (not readable),
 * each keyed half the time */
static void test_scoring(void) {
    int64_t t0 = (int64_t)1718913600 * 1000;

    beacons_reset();
    for (int64_t ms = t0; ms < t0 + 2 * BEACON_ROUND_S * 1000 + BEACON_SLOT_MS; ms += 60) {
        int   b   = beacon_on(0, (time_t)(ms / 1000));
        bool  key = (ms / 300) % 2;
        float db  = -110 + noise(1.5);

        if (key && b == 13) db = -88 + noise(1.0);
        if (key && b == 5) db = -106 + noise(1.0);
        beacons_sample(ms, 0, db);
    }
    const beacon_heard_t *h = beacons_heard(13, 0);
    CHECK(h->heard && abs(h->snr - 22) <= 3, "OH2B heard %d, %d dB", h->heard, h->snr);
    CHECK(!beacons_heard(5, 0)->heard, "VK6RBP not readable (%d dB)", beacons_heard(5, 0)->snr);
    CHECK(!beacons_heard(0, 0)->heard && beacons_heard(0, 0)->snr != BEACON_NONE, "4U1UN listened to, not heard");
    CHECK(beacons_heard(13, 1)->snr == BEACON_NONE, "17 m never listened to");
    CHECK(fabsf(beacons_floor(0) + 110 - 1.9f) < 1.5f, "floor %.1f", beacons_floor(0));

    int heard = 0;
    for (int i = 0; i < BEACONS_N; i++) heard += beacons_heard(i, 0)->heard;
    CHECK(heard == 1, "only OH2B heard, %d", heard);

    /* A wide open band: 12 of the 18 heard, 10 to 32 dB; the noise stays where it is */
    beacons_reset();
    for (int64_t ms = t0; ms < t0 + 2 * BEACON_ROUND_S * 1000 + BEACON_SLOT_MS; ms += 60) {
        int   b  = beacon_on(0, (time_t)(ms / 1000));
        float db = -110 + noise(1.5);

        if ((ms / 300) % 2 && b < 12) db = -110 + 10 + 2 * b + noise(1.0);
        beacons_sample(ms, 0, db);
    }
    heard = 0;
    for (int i = 0; i < BEACONS_N; i++) heard += beacons_heard(i, 0)->heard;
    CHECK(heard == 12, "open band: %d heard", heard);
    CHECK(abs(beacons_heard(11, 0)->snr - 31) <= 3, "open band: 5Z4B %d dB", beacons_heard(11, 0)->snr);

    /* A slot that changed band is not scored */
    beacons_reset();
    int64_t s0 = t0 + 13 * BEACON_SLOT_MS;
    for (int64_t ms = s0; ms < s0 + BEACON_SLOT_MS; ms += 60) beacons_sample(ms, ms < s0 + 5000 ? 0 : 1, -80);
    beacons_sample(s0 + BEACON_SLOT_MS + 600, 1, -110);
    CHECK(beacons_heard(13, 0)->snr == BEACON_NONE && beacons_heard(12, 1)->snr == BEACON_NONE, "mixed slot dropped");
}

/* Ten frequencies, quiet at -115 dB; the 5th busy (-90) from 4 s on for 3 s */
static int scan_run(float stop_db, int *stops, int64_t *hold_ms) {
    int64_t now = 0, held_from = -1;
    int     cur = -1, n_stops = 0;

    scan_init(10);
    scan_config(200, 2000, stop_db, 10);
    scan_start(0, now);
    *hold_ms = 0;
    for (; now < 14000; now += 10) {
        int t = scan_tick(now, 0);
        if (t >= 0) cur = t;
        if (now % 50 == 0) {   /* the S-meter, 20 a second */
            bool busy = cur == 4 && now >= 4000 && now < 7000;
            scan_reading(now, (busy ? -90 : -115) + (float)noise(1.0));
        }
        if (scan_state() == SCAN_HOLD) {
            if (held_from < 0) {
                held_from = now;
                n_stops++;
            }
            CHECK(scan_current() == 4, "held on %d", scan_current());
        } else if (held_from >= 0) {
            *hold_ms += now - held_from;
            held_from = -1;
        }
    }
    *stops = n_stops;
    return scan_passes();
}

static void test_scan(void) {
    int     stops;
    int64_t hold;

    int passes = scan_run(SCAN_AUTO, &stops, &hold);
    CHECK(stops == 1, "auto: %d stops", stops);
    CHECK(hold >= 3500 && hold <= 4500, "auto: held %lld ms (from its next look, about 5 s, to 7 s and the 2 s hang)",
          (long long)hold);
    CHECK(passes >= 4, "auto: %d passes", passes);
    CHECK(scan_hits_n() == 1 && scan_hit(0)->idx == 4 && scan_hit(0)->peak > -93, "auto: the hit");
    CHECK(fabsf(scan_quiet(0) + 115) < 3, "quiet %.1f", scan_quiet(0));

    scan_run(-100, &stops, &hold);
    CHECK(stops == 1, "fixed: %d stops", stops);

    /* Locked out, never stopped on */
    scan_init(10);
    scan_config(200, 2000, -100, 10);
    scan_lockout(4, true);
    scan_start(0, 0);
    int64_t now;
    int     cur = -1, on4 = 0;
    for (now = 0; now < 8000; now += 10) {
        int t = scan_tick(now, 0);
        if (t >= 0) cur = t;
        if (cur == 4) on4++;
        if (now % 50 == 0) scan_reading(now, cur == 4 ? -80 : -115);
    }
    CHECK(on4 == 0 && scan_state() == SCAN_SEEKING, "locked out: %d", on4);

    /* A carrier there from the start is taken for the quiet level of that one, once the
     * others are not known: on a first pass nothing stops */
    scan_init(3);
    scan_config(200, 2000, SCAN_AUTO, 10);
    scan_start(0, 0);
    cur = -1;
    for (now = 0; now < 600; now += 10) {
        int t = scan_tick(now, 0);
        if (t >= 0) cur = t;
        if (now % 50 == 0) scan_reading(now, cur == 0 ? -80 : -115);
    }
    CHECK(scan_hits_n() == 0, "first pass learns");

    /* Skip leaves a hold */
    scan_init(3);
    scan_config(200, 5000, -100, 10);
    scan_start(0, 0);
    cur = -1;
    for (now = 0; now < 1000; now += 10) {
        int t = scan_tick(now, 0);
        if (t >= 0) cur = t;
        if (now % 50 == 0) scan_reading(now, cur == 1 ? -80 : -115);
    }
    CHECK(scan_state() == SCAN_HOLD && scan_current() == 1, "held on 1");
    scan_skip(now);
    CHECK(scan_tick(now, 0) == 2, "skipped to 2");
}

int main(void) {
    srand(1);
    test_sun();
    test_schedule();
    test_scoring();
    test_scan();
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
