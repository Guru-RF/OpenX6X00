/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Band conditions: N0NBH's solar data and band forecast (www.hamqsl.com) through
 *  /usr/sbin/x6100-conditions, fetched in a thread once an hour at most and kept on the card, so
 *  the last ones show without a network too
 */

#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COND_GROUPS 4     /* 80m-40m, 30m-20m, 17m-15m, 12m-10m */

typedef enum { COND_UNKNOWN = -1, COND_POOR, COND_FAIR, COND_GOOD } cond_rating_t;

typedef struct {
    bool          have;
    time_t        fetched;          /* when this was read from the site */
    char          updated[32];      /* as the site has it: "26 Oct 2026 2354 GMT" */
    int           sfi, ssn, a, k;   /* -1 unknown */
    char          xray[8], geomag[16], noise[12], es_na[20];
    cond_rating_t band[COND_GROUPS][2];     /* [group][0 day, 1 night] */
} conditions_t;

extern const char *cond_group_name[COND_GROUPS];   /* "80-40m"... */

/* What is known now (the card's copy at first) */
void conditions_get(conditions_t *c);

/* A fetch in the background when the last one is an hour old (force: 5 minutes) */
void conditions_refresh(bool force);

/* "" idle, "Fetching", "No answer" (the last fetch failed) */
const char *conditions_state(void);

/* Poor, Fair, Good */
const char *cond_rating_name(cond_rating_t r);

/* The forecast group of a frequency, -1 none (160 m, 60 m, 6 m) */
int cond_group_of(int freq_hz);

/* The K index in words: "quiet", "unsettled", "active", "storm" */
const char *cond_k_word(int k);

/* The parser, for the tests: one line of the helper's output into c */
void conditions_parse_line(conditions_t *c, char *line);
void conditions_clear(conditions_t *c);

#ifdef __cplusplus
}
#endif
