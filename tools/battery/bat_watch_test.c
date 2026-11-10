/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/bat_watch.c (#160): the low-battery power-off as radio.c feeds it, one reading of the base's
 *  supply every 0.4 s. Built as the X6100 (6.0 V) and as the X6200 (BOARD_X6200, 7.0 V): not before the readings
 *  count, at BAT_LOW three readings in a row, back off at BAT_HIGH, not on external power (on the X6200 12.0 V and
 *  more on DC IN); and on the X6200 #160's discharges, on battery and on a charger under 12 V, the power-off's 30 s
 *  and the save done before the base's own cut, and a supply close to 12.0 V or one that sags on TX. Then the
 *  voltage's % (bat_watch_cap) on a 0.1 V step.
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
static bool        on_tx;   /* the readings feed() gives are on TX */
static bool        no_bat;  /* and with the base's no battery */

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
    now    = T0;
    on_tx  = false;
    no_bat = false;
}

/* Readings every READ_MS for ms: when low first changed, in ms into the stretch, or NONE */
static long feed(uint8_t vbat, uint8_t vext, bool flag, bool charging, long ms) {
    long changed = NONE;

    for (long t = 0; t < ms; t += READ_MS, now += READ_MS) {
        if (bat_watch_reading(&w, vbat, vext, flag, charging, no_bat, on_tx, now) && changed == NONE) {
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
    check(BAT_LOW == 70 && BAT_HIGH == 72, "the X6200: the power-off at 7.0 V, back off at 7.2 V", BAT_LOW);
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

    /* On battery the readings on TX count: a TX that sags it to BAT_LOW starts it, BAT_HIGH at rest after it calls it
     * off, one step under that does not */
    settled(BAT_HIGH, 0);
    on_tx = true;
    t     = feed(BAT_LOW, 0, false, false, 15000);
    on_tx = false;
    check(t == 2 * READ_MS && w.low, "a TX that sags the battery to BAT_LOW: the power-off", t);
    t = feed(BAT_HIGH, 0, false, false, 5000);
    check(t == 0 && !w.low, "then BAT_HIGH at rest: back at once", t);
    settled(BAT_HIGH - 1, 0);
    on_tx = true;
    feed(BAT_LOW, 0, false, false, 15000);
    on_tx = false;
    check(feed(BAT_HIGH - 1, 0, false, false, 60000) == NONE && w.low, "the same from one step under BAT_HIGH: still low",
          0);

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
    settled(BAT_LOW + 5, 120);
    check(feed(BAT_LOW, 120, true, true, 600000) == NONE, "at BAT_LOW on 12.0 V: never", 0);

#ifdef BOARD_X6200
    /* Under 12.0 V on DC IN the X6200's battery ran the radio (#160): whatever the flags say (its charging flag comes
     * with any supply), the power-off as on battery; plugged in once it runs, it goes on; 12.0 V and more ends it */
    settled(BAT_LOW + 5, 0);
    t = feed(BAT_LOW, 117, true, true, 5000);
    check(t == 2 * READ_MS && w.low && w.ext, "the X6200 at 7.0 V on 11.7 V, the flags on: the power-off", t);
    settled(BAT_LOW + 5, 0);
    t = feed(BAT_LOW, 119, false, false, 5000);
    check(t == 2 * READ_MS && w.low, "the X6200 at 7.0 V on 11.9 V without the flags: the power-off", t);
    check(feed(BAT_LOW, 116, true, true, 60000) == NONE && w.low, "then a charger at 11.6 V: still low", 0);
    t = feed(BAT_LOW, 120, true, true, 10000);
    check(t >= BAT_HELD_MS && t < BAT_HELD_MS + READ_MS && !w.low, "then 12.0 V: back after 5 s", t);
    t = feed(BAT_LOW, 116, true, true, 5000);
    check(t == 4 * READ_MS && w.low, "11.6 V again: the power-off at the fifth reading", t);
    t = feed(BAT_LOW, 122, true, true, 5000);
    check(t == 0 && !w.low, "then 12.2 V: back at once", t);
    check(feed(BAT_LOW - 3, 138, true, false, 600000) == NONE, "a flat battery on 13.8 V, Charger Off: never", 0);

    /* DC IN close to 12.0 V comes on both sides of it, 0.1 V steps: one start of the power-off, and the radio off.
     * Taken from each reading, 11.9 V three times and 12.0 V once started and ended it 75 times in 2 min, each with
     * a 4 s message, and with every other reading at 12.0 V it never started */
    for (int pattern = 0; pattern < 2; pattern++) {
        int changes = 0;

        settled(BAT_LOW + 5, 119);
        for (int i = 0; i < 300; i++) {     /* 2 min */
            changes += feed(BAT_LOW, (pattern ? i % 2 : i % 4 == 3) ? 120 : 119, true, true, READ_MS) != NONE;
        }
        check(changes == 1 && w.low,
              pattern ? "DC IN 11.9/12.0 V in turn: one start, the radio off"
                      : "DC IN 11.9 V three times, 12.0 V once: one start, the radio off", changes);
    }

    /* A flat battery on a supply at 12.2 or 12.0 V that sags to 11.8 V on each TX, FT8's 15 s on and 15 s off, the
     * battery sagging with it: never, it is not started and called off at each TX */
    for (uint8_t v = 120; v <= 122; v += 2) {
        long changed = NONE;

        settled(BAT_LOW, v);
        for (int i = 0; i < 20 && changed == NONE; i++) {   /* 10 min */
            on_tx   = true;
            changed = feed(BAT_LOW - 2, 118, true, true, 15000);
            on_tx   = false;
            if (changed == NONE) changed = feed(BAT_LOW, v, true, true, 15000);
        }
        check(changed == NONE, v > 120 ? "on 12.2 V, 11.8 V on each TX: never" : "on 12.0 V, 11.8 V on each TX: never",
              changed);
    }

    /* The supply gone: at once, also on TX; one that falls under 12.0 V on receive, after three readings */
    settled(BAT_LOW, 138);
    on_tx = true;
    t     = feed(BAT_LOW, 0, false, false, 5000);
    check(t == 2 * READ_MS && w.low, "the supply pulled on TX: the power-off at the third reading", t);
    settled(BAT_LOW, 122);
    t = feed(BAT_LOW, 118, true, true, 5000);
    check(t == 4 * READ_MS && w.low, "12.2 V, then 11.8 V on receive: the power-off at the fifth reading", t);

    /* The base finds no battery: never, whatever vbat says (the stock charger without a pack); ends a running one */
    start();
    no_bat = true;
    check(feed(50, 118, true, true, 600000) == NONE, "no battery found, 5.0 V on vbat, on 11.8 V: never", 0);
    settled(BAT_LOW + 5, 118);
    feed(BAT_LOW, 118, true, true, 5000);
    no_bat = true;
    t      = feed(BAT_LOW, 118, true, true, 5000);
    check(t == 0 && !w.low, "low on 11.8 V, then no battery found: back at once", t);

    /* #160, 2 November: switched on again on the charger (11.6-11.8 V), the battery fell from 6.9 to 6.4 V in
     * 2 minutes and the base cut the power. Here 0.1 V every 24 s from the first reading, the cut at 6.4 V */
    start();
    long low_on = NONE;

    for (int step = 0; step < 5 && low_on == NONE; step++) {
        if ((t = feed(69 - step, step % 2 ? 116 : 118, true, true, 24000)) != NONE) low_on = step * 24000 + t;
    }
    check(low_on != NONE && low_on + OFF_MS < 120000 - 30000, "#160 on the charger at 11.6-11.8 V: off before the cut",
          low_on == NONE ? -1 : (120000 - low_on - OFF_MS) / 1000);
    printf("#160 on the charger: \"Low battery!\" %.1f s after the first reading, the radio off %ld s before the cut\n",
           low_on / 1000.0, (120000 - low_on - OFF_MS) / 1000);
#else
    /* The X6100 on a supply whatever its voltage: 11.7 V on the jack and both flags clear (#17's charger) */
    settled(BAT_LOW + 5, 0);
    check(feed(BAT_LOW - 1, 117, false, false, 600000) == NONE, "the X6100 on 11.7 V, the flags clear: never", 0);
#endif

#ifdef BOARD_X6200
    /* #160, 1 and 2 November: on battery, receive, WiFi and BT on. The first 6.8 V reading 4-5 min before the base cut
     * the power, 6.7 V about 2.5 min, the last log rows 6.6 and 6.4 V; 6 November, the same load: 7.0 V 3 min before
     * 6.8 V. Here the shorter 4 min, the voltage first on the 7.1/7.0 V step for a minute, as it goes back and forth
     * on a step, then 7.0 V for 1.5 min and 6.9 V for 30 s */
    settled(73, 0);
    feed(71, 0, false, false, 300000);
    long first70 = now - T0, low_at = NONE;

    for (int i = 0; i < 150 && low_at == NONE; i++) {      /* 7.0, 7.1, 7.0, 7.0, 7.1 ... for 60 s */
        if (feed(i % 3 == 1 ? 71 : 70, 0, false, false, READ_MS) != NONE) low_at = now - T0 - READ_MS;
    }
    if (low_at == NONE && (t = feed(70, 0, false, false, 90000)) != NONE) low_at = now - T0 - 90000 + t;
    if (low_at == NONE && (t = feed(69, 0, false, false, 30000)) != NONE) low_at = now - T0 - 30000 + t;
    if (low_at == NONE && (t = feed(68, 0, false, false, 240000)) != NONE) low_at = now - T0 - 240000 + t;
    long cut = first70 + 180000 + 240000;

    check(low_at != NONE && low_at + OFF_MS < cut - 300000, "#160's discharge: off 5 min or more before the cut",
          low_at == NONE ? -1 : (cut - low_at - OFF_MS) / 1000);
    printf("#160's discharge: \"Low battery!\" %.1f s after the first 7.0 V, the radio off %ld s before the cut\n",
           (low_at - first70) / 1000.0, (cut - low_at - OFF_MS) / 1000);
#else
    /* The X6100 runs to 6.0 V, as before */
    settled(70, 0);
    check(feed(61, 0, false, false, 600000) == NONE, "the X6100 at 6.1 V: running", 0);
#endif

    /* The voltage's % on a 0.1 V step (#160: 24 and 9 % in turn), with a base stuck at 0 % (#49) so that the voltage's
     * is shown: down at once, back up 0.1 V after BAT_CAP_UP_READS readings in a row over it, 0.2 V at once */
    settled(73, 0);
    check(bat_watch_cap(&w, 73, 0) == 24, "7.3 V: 24 %", w.cap_vbat);
    check(bat_watch_cap(&w, 72, 0) == 9, "7.2 V: 9 % at once", w.cap_vbat);
    int flips = 0;

    for (int i = 0; i < 1000; i++) {    /* 7.3, 7.2, 7.3, 7.3, 7.2 ... for some 7 min, the last 7.2 */
        flips += bat_watch_cap(&w, i % 3 == 1 || i == 999 ? 72 : 73, 0) != 9;
    }
    check(flips == 0, "on the 7.3/7.2 V step: 9 % throughout", flips);
    for (int i = 1; i < BAT_CAP_UP_READS; i++) {
        flips += bat_watch_cap(&w, 73, 0) != 9;
    }
    check(flips == 0, "7.3 V again: 9 % for a minute", flips);
    check(bat_watch_cap(&w, 73, 0) == 24, "then 24 %", w.cap_vbat);
    check(bat_watch_cap(&w, 71, 0) == 3 && bat_watch_cap(&w, 73, 0) == 24, "7.1 V, then 7.3 V: 24 % at once", 0);

    /* A TX: the % held (radio.c), then the battery a step under for a while as it comes back */
    check(bat_watch_cap(&w, 72, 0) == 9, "after a TX at 7.2 V: 9 %", 0);
    for (int i = 0; i < BAT_CAP_UP_READS; i++) {
        bat_watch_cap(&w, 73, 0);
    }
    check(bat_watch_cap(&w, 73, 0) == 24, "back at 7.3 V for a minute: 24 % again", w.cap_vbat);

    /* External power: from the first reading on battery again, as it is */
    check(bat_watch_cap(&w, 72, 0) == 9, "7.2 V: 9 %", 0);
    feed(73, 138, true, true, 5000);
    feed(73, 0, false, false, READ_MS);
    check(bat_watch_cap(&w, 73, 0) == 24, "a short charge to 7.3 V: 24 % at once", w.cap_vbat);

    /* A base that counts right (the bench X6100, 1.1.8) is shown as it is, on the step too */
    settled(73, 0);
    check(bat_watch_cap(&w, 73, 26) == 26 && bat_watch_cap(&w, 72, 20) == 20 && bat_watch_cap(&w, 73, 19) == 19,
          "a base that counts right: its own %", 0);

    printf("bat_watch (%s): %d checks, %d failed\n", BOARD_BAT_LOW == 60 ? "X6100" : "X6200", checks, fails);
    return fails != 0;
}
