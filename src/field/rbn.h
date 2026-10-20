/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: Reverse Beacon Network client. A thread logs in
 *  to telnet.reversebeacon.net:7000 with the callsign and keeps, from the skimmer
 *  spots, the activity per band (spots in the last minutes) and the skimmers that
 *  heard our callsign.
 */

#pragma once

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIELD_RBN_BANDS   11    /* field_band_of() indexes: 160 m .. 6 m */
#define FIELD_RBN_MINUTES 15    /* activity window */

typedef struct {
    char    skimmer[16];
    int32_t freq_hz;
    int     snr;
    int     wpm;
    time_t  when;
} field_rbn_hit_t;

/* Starts the client for the callsign (again with a new one); stop ends it */
void field_rbn_start(const char *call);
void field_rbn_stop(void);

/* "off", "connecting", "connected", "no answer", ... */
const char *field_rbn_state(void);

/* Skimmers that heard the callsign in the last max_age_s, newest first, each once */
int field_rbn_heard(int max_age_s, field_rbn_hit_t *out, int max);

/* Skimmer spots per band in the last FIELD_RBN_MINUTES */
void field_rbn_activity(int counts[FIELD_RBN_BANDS]);

/* One line as if from the server (tests) */
void field_rbn_feed_line(const char *line, time_t now);

/* The same station: ON6URE, ON6URE/P, F/ON6URE... */
int field_same_call(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
