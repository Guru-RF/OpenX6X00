/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the clock in the field: the rule (src/clock_rule.c) and a time station's minute marker
 *  (src/time_station.c) on audio made up here. See the Makefile.
 */

#include "clock_sync.h"
#include "time_station.h"

#include <math.h>
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

/* ---- the rule ---- */

static void test_keeper(void) {
    CHECK(clock_sync_keeper_of(true, 0, false) == CLOCK_BY_GPS, "a recent GPS fix keeps the clock");
    CHECK(clock_sync_keeper_of(true, 1, true) == CLOCK_BY_GPS, "the GPS first");
    CHECK(clock_sync_keeper_of(false, 1, false) == CLOCK_BY_NTP, "ntpd synchronised: NTP, the kernel unsure (a step)");
    CHECK(clock_sync_keeper_of(false, 0, true) == CLOCK_BY_NONE, "ntpd says no source: nothing, whatever the kernel");
    CHECK(clock_sync_keeper_of(false, -1, true) == CLOCK_BY_NTP, "no ntpd answer: the kernel's word");
    CHECK(clock_sync_keeper_of(false, -1, false) == CLOCK_BY_NONE, "no answer, the kernel unsynced: nothing");
}

static void test_may_step(void) {
    const clock_by_t movers[] = {CLOCK_BY_HAND, CLOCK_BY_STATION, CLOCK_BY_FT8};

    /* GPS and NTP keep it: nobody moves it, Auto Sync or not */
    for (size_t i = 0; i < 3; i++) {
        CHECK(!clock_sync_may_step(movers[i], CLOCK_BY_GPS, CLOCK_BY_NONE, -1, true), "%s: not under the GPS",
              clock_sync_by_name(movers[i]));
        CHECK(!clock_sync_may_step(movers[i], CLOCK_BY_NTP, CLOCK_BY_NONE, -1, true), "%s: not under NTP",
              clock_sync_by_name(movers[i]));
    }

    /* nothing keeps it */
    CHECK(clock_sync_may_step(CLOCK_BY_HAND, CLOCK_BY_NONE, CLOCK_BY_NONE, -1, false), "Time Sync by hand: yes");
    CHECK(clock_sync_may_step(CLOCK_BY_STATION, CLOCK_BY_NONE, CLOCK_BY_NONE, -1, false),
          "a time station listened to: yes, Auto Sync off too");
    CHECK(clock_sync_may_step(CLOCK_BY_STATION, CLOCK_BY_NONE, CLOCK_BY_FT8, 10, false), "a station after FT8: yes");
    CHECK(!clock_sync_may_step(CLOCK_BY_FT8, CLOCK_BY_NONE, CLOCK_BY_NONE, -1, false), "FT8 with Auto Sync off: no");
    CHECK(clock_sync_may_step(CLOCK_BY_FT8, CLOCK_BY_NONE, CLOCK_BY_NONE, -1, true), "FT8 with Auto Sync on: yes");
    CHECK(clock_sync_may_step(CLOCK_BY_FT8, CLOCK_BY_NONE, CLOCK_BY_FT8, 60, true), "FT8 after FT8: yes");
    CHECK(clock_sync_may_step(CLOCK_BY_FT8, CLOCK_BY_NONE, CLOCK_BY_HAND, 60, true), "FT8 after Time Sync: yes");
    CHECK(!clock_sync_may_step(CLOCK_BY_FT8, CLOCK_BY_NONE, CLOCK_BY_STATION, 600, true),
          "FT8 10 min after a time station: no (the station is nearer the truth)");
    CHECK(!clock_sync_may_step(CLOCK_BY_FT8, CLOCK_BY_NONE, CLOCK_BY_STATION, CLOCK_SYNC_STATION_HOLD_S - 1, true),
          "FT8 just under an hour after a station: no");
    CHECK(clock_sync_may_step(CLOCK_BY_FT8, CLOCK_BY_NONE, CLOCK_BY_STATION, CLOCK_SYNC_STATION_HOLD_S, true),
          "FT8 an hour after a station: yes");
    CHECK(!clock_sync_may_step(CLOCK_BY_GPS, CLOCK_BY_NONE, CLOCK_BY_NONE, -1, true) &&
              !clock_sync_may_step(CLOCK_BY_NTP, CLOCK_BY_NONE, CLOCK_BY_NONE, -1, true),
          "GPS and NTP move no clock here (ntpd does)");
}

/* ntpd's answer: a mode 6 header, its status word's first byte the leap bits and the sync source */
static int ntpd(uint8_t mode_byte, uint8_t op_byte, int leap, int source, size_t n) {
    uint8_t r[48] = {0};

    r[0] = mode_byte;
    r[1] = op_byte;
    r[3] = 1;
    r[4] = (uint8_t)(leap << 6 | source);
    return clock_sync_ntpd_reply(r, n);
}

static void test_ntpd(void) {
    CHECK(ntpd(0x16, 0x82, 0, 6, 48) == 1, "synchronised to a server (source 6, NTP)");
    CHECK(ntpd(0x16, 0x82, 0, 2, 48) == 1, "synchronised to a reference clock (source 2, the GPS through SHM)");
    CHECK(ntpd(0x16, 0x82, 1, 6, 48) == 1, "a leap second announced: still synchronised");
    CHECK(ntpd(0x16, 0x82, 3, 6, 48) == 0, "leap bits 3: not synchronised");
    CHECK(ntpd(0x16, 0x82, 0, 5, 48) == 0, "its local clock: no source");
    CHECK(ntpd(0x16, 0x82, 0, 0, 48) == 0, "source unspecified: none");
    CHECK(ntpd(0x16, 0x02, 0, 6, 48) == -1, "a question, not an answer");
    CHECK(ntpd(0x24, 0x82, 0, 6, 48) == -1, "mode 4, not 6");
    CHECK(ntpd(0x16, 0x81, 0, 6, 48) == -1, "an answer to another op");
    CHECK(ntpd(0x16, 0x82, 0, 6, 8) == -1, "too short");
}

static void test_learn(void) {
    clock_learned_t l = {0.15f, 0};

    for (int i = 0; i < 5; i++)
        clock_sync_learn(&l, i % 2 ? 0.30f : 0.20f, 0.3f);
    CHECK(l.n == 5 && fabsf(l.value - 0.24f) < 1e-4f, "the first five averaged: %.3f", l.value);
    for (int i = 0; i < 5; i++)
        clock_sync_learn(&l, 0.24f, 0.3f);
    CHECK(clock_sync_learn(&l, 0.44f, 0.3f) && fabsf(l.value - 0.25f) < 1e-3f, "then 5 %% of each: %.3f", l.value);
    CHECK(!clock_sync_learn(&l, 0.80f, 0.3f) && l.n == 11, "one far off, ten in: left out");
    CHECK(!clock_sync_learn(&l, NAN, 0.3f), "no number: left out");
}

/* ---- the minute marker, on audio made up here ---- */

#define FS      TIME_STATION_FS
#define T0      1763290800.0        /* a minute's start (true time): 2026-11-16 11:00 UTC */

typedef enum { RWM, WWV, WWVH, CHU, NONE } kind_t;

/* The station's tone on (1) or off (0), true time t s */
static int keyed(kind_t k, double t) {
    double s   = fmod(t, 60.0);     /* into the minute */
    int    sec = (int)s;
    double in  = s - sec;           /* into the second */

    switch (k) {
        case RWM:  return sec == 0 ? in < 0.5 : in < 0.1;              /* the minute 500 ms, the seconds 100 ms */
        case WWV:
        case WWVH: return sec == 0 ? in < 0.8 : (sec != 29 && sec != 59 && in < 0.005);   /* ticks 5 ms */
        case CHU:
            if (sec == 0)
                return in < 0.5;
            if (sec == 29 || sec >= 51)
                return 0;
            return sec >= 31 && sec <= 39 ? in < 0.01 : in < 0.3;
        default:   return 0;
    }
}

static double gauss(void) {
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);

    return sqrt(-2.0 * log(u)) * cos(2 * M_PI * v);
}

/* Feeds `minutes` of the station (from T0 - 20 s, true time) to the detector listening to `listen`: the clock ahead
 * by `ahead`, the signal `delay` late (the path), blocks of 256 samples coming 0 to `late` s after their last sample */
static void feed(kind_t k, int listen, double ahead, double delay, double late, double minutes, double snr_amp) {
    float  buf[256];
    double tone = k == WWVH ? 1200.0 : 1000.0;
    long   n    = (long)((minutes * 60.0 + 20.0) * FS);

    time_station_start(listen);
    for (long i = 0; i < n; i += 256) {
        for (int j = 0; j < 256; j++) {
            double t = T0 - 20.0 + (i + j) / FS;      /* true time of this sample */
            double a = k != NONE && keyed(k, t - delay) ? snr_amp : 0.0;

            buf[j] = (float)(a * sin(2 * M_PI * tone * t) + 0.05 * gauss());
        }

        double last = T0 - 20.0 + (i + 255) / FS;

        time_station_audio_at(256, buf, last + ahead + late * (rand() / (double)RAND_MAX));
    }
}

static int station(const char *name) {
    for (int i = 0; i < time_stations_n; i++)
        if (!strcmp(time_stations[i].name, name))
            return i;
    return -1;
}

static void test_marker(void) {
    time_station_state_t st;

    srand(3);

    struct {
        kind_t      k;
        const char *listen;
        double      ahead, delay;
    } cases[] = {
        {RWM, "RWM 9996", 0.237, 0.012},
        {RWM, "RWM 4996", -1.480, 0.004},
        {WWV, "WWV 10000", -0.083, 0.031},
        {CHU, "CHU 7850", 4.5, 0.020},
        {WWVH, "WWVH 15000", 0.6, 0.045},
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int i = station(cases[c].listen);

        CHECK(i >= 0, "%s is a station", cases[c].listen);
        feed(cases[c].k, i, cases[c].ahead, cases[c].delay, 0.015, 3.2, 0.3);
        time_station_get(&st);

        double want = -(cases[c].ahead + cases[c].delay);

        printf("  %s, the clock %+.3f s, the path %.3f s: %d markers, %s raw %+.4f s (want %+.4f), %.0f dB\n",
               cases[c].listen, cases[c].ahead, cases[c].delay, st.markers, st.result ? "result" : "no result", st.raw,
               want, st.snr_db);
        CHECK(st.markers == 4, "%s: the four minute markers, no second pulse (%d)", cases[c].listen, st.markers);
        CHECK(st.result && fabs(st.raw - want) < 0.003, "%s: the clock found to 3 ms (%+.4f, want %+.4f)",
              cases[c].listen, st.raw, want);
    }

    /* WWVH's 1200 Hz to a WWV listener (1000 Hz): nothing; WWV's to a WWVH listener neither */
    feed(WWVH, station("WWV 5000"), 0.1, 0.0, 0.01, 2.2, 0.3);
    time_station_get(&st);
    CHECK(!st.result && st.markers == 0, "WWVH unheard on WWV's tone (%d markers)", st.markers);
    feed(WWV, station("WWVH 5000"), 0.1, 0.0, 0.01, 2.2, 0.3);
    time_station_get(&st);
    CHECK(!st.result && st.markers == 0, "WWV unheard on WWVH's tone (%d markers)", st.markers);

    /* noise only */
    feed(NONE, station("RWM 9996"), 0.0, 0.0, 0.01, 2.2, 0.0);
    time_station_get(&st);
    CHECK(!st.result && st.markers == 0, "noise: no marker (%d)", st.markers);

    /* a weak station still: the tone at the noise's level */
    feed(RWM, station("RWM 14996"), 0.05, 0.0, 0.01, 3.2, 0.05);
    time_station_get(&st);
    CHECK(st.result && fabs(st.raw + 0.05) < 0.005, "a weak RWM: found (%+.4f, %.0f dB)", st.raw, st.snr_db);

    /* a restart (the clock set): the result goes, the markers start again */
    time_station_restart();
    {
        float b[256] = {0};

        time_station_audio_at(256, b, T0);
    }
    time_station_get(&st);
    CHECK(st.on && !st.result && st.markers == 0, "after a restart: no result until two new markers");

    time_station_stop();
    time_station_get(&st);
    CHECK(!st.on, "stopped");
}

static void test_quiet(void) {
    const time_station_t *rwm = &time_stations[station("RWM 9996")], *wwv = &time_stations[station("WWV 10000")];

    CHECK(time_station_quiet_until(rwm, 0) == 10 && time_station_quiet_until(rwm, 9) == 10, "RWM :00-:09: until :10");
    CHECK(time_station_quiet_until(rwm, 10) == -1 && time_station_quiet_until(rwm, 29) == -1, "RWM :10-:29: markers");
    CHECK(time_station_quiet_until(rwm, 30) == 40 && time_station_quiet_until(rwm, 39) == 40, "RWM :30-:39: until :40");
    CHECK(time_station_quiet_until(rwm, 45) == -1, "RWM :45: markers");
    CHECK(time_station_quiet_until(wwv, 5) == -1, "WWV: every minute");
}

int main(void) {
    test_keeper();
    test_may_step();
    test_ntpd();
    test_learn();
    test_marker();
    test_quiet();
    printf("clock: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
