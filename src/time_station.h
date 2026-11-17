/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a time station's minute marker: the moment the clock should read :00, for the clock in the field
 *  (clock_sync.h, the GPS / Clock page). RWM (Moscow) keys its carrier 500 ms on the minute in minutes :10-:29 and
 *  :40-:59, CHU (Ottawa) sends a 1000 Hz tone 500 ms, WWV (Colorado) 1000 Hz and WWVH (Hawaii) 1200 Hz 800 ms. In
 *  USB on the dial given each comes out as its tone. A marker found to a few ms; which minute is the clock's own, so
 *  the clock must already be within 30 s (the clock chip keeps it so). Two markers 1 to 5 minutes apart that agree
 *  within 30 ms make a result.
 *
 *  From OlivLoze's fork (F4OLI): the detector and RWM and WWV; CHU and WWVH added here (#261, #275).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The audio's rate: the apps' (AUDIO_CAPTURE_RATE / AUDIO_DECIM) */
#define TIME_STATION_FS     11025.0

typedef struct {
    const char *name;       /* "RWM 9996" */
    int32_t     freq_hz;    /* the station */
    int32_t     dial_hz;    /* USB dial: the marker at tone_hz */
    int         tone_hz;
    int         marker_ms;
    bool        quiet;      /* RWM: no markers in minutes :00-:09 and :30-:39 */
} time_station_t;

extern const time_station_t time_stations[];
extern const int            time_stations_n;

/* Listens for station i's markers (the radio tuned by the caller); stop. Any thread */
void time_station_start(int i);
void time_station_stop(void);

/* Every marker so far was of the clock before (it was set): start over. Any thread */
void time_station_restart(void);

/* The receiver's audio at TIME_STATION_FS (audio thread), the clock read as it comes */
void time_station_audio(unsigned int n, float *samples);

/* The same with the clock given (CLOCK_REALTIME, s) as the block came: the host test */
void time_station_audio_at(unsigned int n, const float *samples, double now);

typedef struct {
    bool     on;
    int      station;
    float    snr_db;        /* the last marker over the noise, 0 none */
    int      markers;       /* heard since the start or the last restart */
    double   marker_at;     /* the last one, by the clock */
    bool     result;        /* two markers agree */
    float    raw;           /* then the minute's start minus the marker, by the clock, s: -(the clock ahead + the
                             * delay from the station to the measure) */
    unsigned result_id;     /* changes with each result */
} time_station_state_t;

void time_station_get(time_station_state_t *st);

/* A quiet station's minutes without markers: the next minute with them (0-59), -1 when this one has them */
int time_station_quiet_until(const time_station_t *s, int minute);

#ifdef __cplusplus
}
#endif
