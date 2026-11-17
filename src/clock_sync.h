/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the clock: what keeps it, and who may move it. One rule for every source:
 *
 *    1. the GPS (a fix of the last 10 s: gpsd feeds ntpd) and NTP (ntpd synchronised to a source) keep the clock:
 *       nothing here moves it then, the others only measure, and learn what a right clock reads by them;
 *    2. a time station's minute marker (time_station.h), while the GPS / Clock page listens to one: it moves the
 *       clock to the ms, and holds FT8 off for an hour after (the FT8 stations' own clocks and our receive path move
 *       FT8's word by a tenth of a second);
 *    3. FT8 and FT4 (ft8/time_sync.h, the decodes' DT): with Auto Sync on (cfg ft8_auto_sync, on the GPS / Clock
 *       page), the clock follows them;
 *    4. by hand: FT8's Time Sync key, pressed as a slot starts.
 *
 *  Whatever moves the clock goes through clock_sync_step: the clock chip too, and the last move named on the page.
 *  What a right clock reads by FT8 (the median DT, FTX_SYNC_RX_DT_S until learned) and by the time station (the
 *  delay from the station to the measure) is learned while NTP keeps the clock, and kept in BOARD_DATA/clock_sync.conf.
 *
 *  The ntpd query, the time station and the learning from OlivLoze's fork (F4OLI); the FT8 part is main's Auto Sync.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLOCK_BY_NONE,          /* nothing keeps the clock: the clock chip's, or the last step's */
    CLOCK_BY_GPS,
    CLOCK_BY_NTP,
    CLOCK_BY_STATION,       /* a time station's minute marker */
    CLOCK_BY_FT8,           /* Auto Sync, the FT8 or FT4 decodes */
    CLOCK_BY_HAND,          /* Time Sync */
} clock_by_t;

#define CLOCK_SYNC_GPS_AGE_S        10      /* a GPS fix this recent keeps the clock */
#define CLOCK_SYNC_STATION_HOLD_S   3600    /* FT8 moves no clock a time station set this recently */

/* ---- the rule, pure (clock_rule.c; tools/clock_sync tests it) ---- */

/* What keeps the clock: a GPS fix of the last CLOCK_SYNC_GPS_AGE_S, else ntpd's word (1 synchronised, 0 not, -1 no
 * answer) and, without an answer, the kernel's (STA_UNSYNC: a step sets it until ntpd's next poll, up to 17 min) */
clock_by_t clock_sync_keeper_of(bool gps_recent, int ntpd, bool kernel_synced);

/* Whether `by` may move a clock that `keeper` keeps, the last move by last_by `since` s ago, Auto Sync on or off */
bool clock_sync_may_step(clock_by_t by, clock_by_t keeper, clock_by_t last_by, long since, bool auto_sync);

/* ntpd's answer to a mode 6 READVAR (ntpq -c rv): 1 synchronised to a source, 0 not (the leap bits 3, or no source,
 * or its local clock), -1 not such an answer */
int clock_sync_ntpd_reply(const uint8_t *r, size_t n);

/* A learned value: the first five measures averaged, then 5 % of each; one more than `far` from it, once ten are in,
 * is left out (false) */
typedef struct {
    float value;
    int   n;
} clock_learned_t;

bool clock_sync_learn(clock_learned_t *l, float v, float far);

const char *clock_sync_by_name(clock_by_t by);

/* ---- the radio ---- */

/* What keeps the clock now (any thread; ntpd asked at most every 10 s). err_ms (may be NULL) gets the kernel's
 * maximum error with NTP, 0 without */
clock_by_t clock_sync_keeper(int *err_ms);

/* Whether `by` may move the clock now, by the rule; why (may be NULL) gets the keeper when it is the reason */
bool clock_sync_allowed(clock_by_t by, clock_by_t *keeper);

/* The clock moved by step s (the clock chip with it), by what, named ("RWM 9996", "8 stations"). No rule asked:
 * the caller did (clock_sync_allowed). False when the clock can't be set. Any thread */
bool clock_sync_step(double step, clock_by_t by, const char *name);

/* The last move here, false none since the start */
bool clock_sync_last(clock_by_t *by, char *name, size_t size, time_t *when, double *step);

/* ---- FT8's word (ft8_window.c, the decoder thread), for the page ---- */

typedef struct {
    bool   valid;
    int    rc;              /* ftx_sync_rc_t */
    float  step;            /* what the clock should move by, s */
    float  median_dt;
    int    stations, slots;
    time_t when;
} clock_ft8_word_t;

void clock_sync_ft8_word(int rc, float step, float median_dt, int stations, int slots);
void clock_sync_ft8_get(clock_ft8_word_t *out);

/* What a right clock reads by the FT8 decodes (median DT, s): learned, else FTX_SYNC_RX_DT_S. n (may be NULL): the
 * minutes learned from */
float clock_sync_ft8_rx_dt(int *n);
/* One more median DT of a clock NTP keeps */
void  clock_sync_ft8_rx_dt_learn(float median_dt);

/* The delay from a time station to the measure (s), learned while NTP keeps the clock; n as above */
float clock_sync_station_delay(int *n);
void  clock_sync_station_delay_learn(float delay);

#ifdef __cplusplus
}
#endif
