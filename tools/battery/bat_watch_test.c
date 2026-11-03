/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/bat_watch.c (#160): the low-battery power-off as radio.c feeds it, one reading of the base's
 *  supply every 0.4 s. Built as the X6100 (6.0 V) and as the X6200 (BOARD_X6200, 6.8 V): not before the readings
 *  count, at BAT_LOW three readings in a row, back off at BAT_HIGH, not on external power; and on the X6200 #160's
 *  discharges, the power-off's 30 s and the save done minutes before the base's own cut.
 */

#include "bat_watch.h"

#include <stdio.h>
#include <string.h>

#define READ_MS 400         /* radio.c: a reading every 12 packets, about 0.4 s */
#define T0      1000000     /* the GUI's start, ms of the monotonic clock */
#define OFF_MS  35000       /* from "Low battery!" to the radio off: the 30 s, then the charger, the save, the sync */
#define NONE    (-1)

static int         fails, checks;
static bat_watch_t w;
static uint64_t    now;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* A fresh start: the first reading at T0 */
static void start(void) {
    memset(&w, 0, sizeof(w));
    now = T0;
}

/* Readings every READ_MS for ms: when low first changed, in ms into the stretch, or NONE */
static long feed(uint8_t vbat, uint8_t vext, bool flag, bool charging, long ms) {
    long changed = NONE;

    for (long t = 0; t < ms; t += READ_MS, now += READ_MS) {
        if (bat_watch_reading(&w, vbat, vext, flag, charging, now) && changed == NONE) {
            changed = t;
        }
    }
    return changed;
}

/* From a start, until the readings count: on battery, or with vext on the jack */
static void settled(uint8_t vbat, uint8_t vext) {
    start();
    feed(vbat, vext, vext >= BAT_NONE, false, BAT_SETTLE_MS + READ_MS);
    check(w.counts && !w.low, "the readings count after the settle", vbat);
}

int main(void) {
    long t;

#ifdef BOARD_X6200
    check(BAT_LOW == 68 && BAT_HIGH == 70, "the X6200: the power-off at 6.8 V, back off at 7.0 V", BAT_LOW);
#else
    check(BAT_LOW == 60 && BAT_HIGH == 62, "the X6100: the power-off at 6.0 V, back off at 6.2 V", BAT_LOW);
#endif

    /* Not before the readings count: a flat battery at power-on waits the settle, then three readings */
    start();
    check(feed(BAT_LOW - 5, 0, false, false, BAT_SETTLE_MS) == NONE, "flat at power-on: not before the settle", 0);
    t = feed(BAT_LOW - 5, 0, false, false, 5000);
    check(t == 2 * READ_MS && w.low, "flat at power-on: the third reading that counts", t);

    /* At BAT_LOW three readings in a row; one over, never; two then one over, not yet */
    settled(BAT_LOW + 5, 0);
    check(feed(BAT_LOW + 1, 0, false, false, 600000) == NONE, "one step over BAT_LOW for 10 min: never", 0);
    feed(BAT_LOW, 0, false, false, 2 * READ_MS);
    feed(BAT_LOW + 1, 0, false, false, READ_MS);
    check(!w.low, "two readings at BAT_LOW, then one over: not yet", w.low_reads);
    t = feed(BAT_LOW, 0, false, false, 5000);
    check(t == 2 * READ_MS && w.low, "at BAT_LOW: the third reading in a row", t);

    /* Back: one step over stays low (the voltage on a step goes back and forth), BAT_HIGH ends it */
    check(feed(BAT_LOW + 1, 0, false, false, 60000) == NONE && w.low, "one step over: still low", 0);
    t = feed(BAT_HIGH, 0, false, false, 5000);
    check(t == 0 && !w.low, "at BAT_HIGH: back at once", t);

    /* No battery, or none measured: never */
    settled(0, 138);
    check(feed(0, 138, true, false, 60000) == NONE, "no battery on a supply: never", 0);
    check(feed(BAT_NONE - 1, 0, false, false, 60000) == NONE, "under 3.0 V: none fitted, never", 0);

    /* External power: the base's flag, its charging flag, or 9.0 V and more on the jack alone */
    settled(BAT_LOW + 5, 0);
    check(feed(BAT_LOW - 3, 138, true, true, 600000) == NONE, "a flat battery on 13.8 V: never", 0);
    check(feed(BAT_LOW - 3, 138, false, false, 600000) == NONE, "on 13.8 V without the flags: never", 0);
    settled(BAT_LOW + 5, 0);
    feed(BAT_LOW, 0, false, false, 5000);
    t = feed(BAT_LOW, 138, true, true, 5000);
    check(t == 0 && !w.low, "low, then a 13.8 V supply: back at once", t);

#ifdef BOARD_X6200
    /* #160, 1 and 2 November: on battery, receive, WiFi and BT on. The first 6.8 V reading 4-5 min before the base cut
     * the power, 6.7 V about 2.5 min, the last log rows 6.6 and 6.4 V. Here the shorter 4 min, the voltage first on the
     * 6.9/6.8 V step for a minute, as the % went back and forth there */
    settled(72, 0);
    feed(69, 0, false, false, 300000);
    long first68 = now - T0, low_at = NONE;

    for (int i = 0; i < 150 && low_at == NONE; i++) {      /* 6.8, 6.9, 6.8, 6.8, 6.9 ... for 60 s */
        if (feed(i % 3 == 1 ? 69 : 68, 0, false, false, READ_MS) != NONE) low_at = now - T0 - READ_MS;
    }
    if (low_at == NONE && (t = feed(68, 0, false, false, 90000)) != NONE) low_at = now - T0 - 90000 + t;
    long cut = first68 + 240000;

    check(low_at != NONE && low_at + OFF_MS < cut - 120000, "#160's discharge: off 2 min or more before the cut",
          low_at == NONE ? -1 : (cut - low_at - OFF_MS) / 1000);
    printf("#160's discharge: \"Low battery!\" %.1f s after the first 6.8 V, the radio off %ld s before the cut\n",
           (low_at - first68) / 1000.0, (cut - low_at - OFF_MS) / 1000);
#else
    /* The X6100 runs to 6.0 V, as before */
    settled(70, 0);
    check(feed(61, 0, false, false, 600000) == NONE, "the X6100 at 6.1 V: running", 0);
#endif

    printf("bat_watch (%s): %d checks, %d failed\n", BOARD_BAT_LOW == 68 ? "X6200" : "X6100", checks, fails);
    return fails != 0;
}
