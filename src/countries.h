/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the countries (ISO 3166-1) and the one the radio is used in
 *
 *  The list (countries_data.inc, made by tools/countries/gen.py from tzdata and the Unicode CLDR): each country's
 *  code, its IARU region, the time zone and the position of its place in the time zone database (the capital or the
 *  largest city), and its name in the GUI's languages, in each language's alphabetical order.
 *
 *  The country the radio is used in is a setting, Settings > General > STATION > Country and the first-start guide
 *  (first_start.c): the code in capitals, "BE", "" when none is set (the default). The band plan's Auto takes its
 *  region when the GPS and the locator do not tell (band_plan.c). Read it with country_now() or country_code(); in
 *  C++ cfg_sm.p_country.get(); the web pages read "country" from params.db.
 *
 *  After the CX8ANM edition of OpenX6X00 (condoruy), whose first-start wizard asked for the country.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char        cc[3];      /* ISO 3166-1 alpha-2, capitals: "BE" */
    uint8_t     region;     /* IARU region, 1-3 */
    const char *tz;         /* the POSIX TZ string of its place, "" none */
    float       lat, lon;   /* that place, degrees north and east (to a minute) */
} country_t;

/* The list. All of these work from any thread */
int              countries_n(void);
/* By its code, in any case; NULL for "" or a code not in the list */
const country_t *country_find(const char *cc);
/* Its name in the GUI's language (i18n_code()), in English when the list has none in it; "" for NULL */
const char      *country_name(const country_t *c);
/* The i-th, 0 to countries_n() - 1, in the alphabetical order of the GUI's language; NULL past the end */
const country_t *country_sorted(int i);
/* The place of the country cc in that order, -1 none */
int              country_sorted_index(const char *cc);

/* The setting: the code into cc (3 bytes) and back, "" when none is set. Any thread */
const char      *country_code(char *cc);
/* The country set, NULL none. Any thread */
const country_t *country_now(void);
/* Set it ("" clears it): false for a code not in the list, nothing changed then. The band plan's Auto follows.
 * LVGL thread */
bool             country_save(const char *cc);

/* The country of a callsign's DXCC entity (cty.dat, src/dxcc.h; the code by flags.h) into cc (3 bytes): false when
 * the callsign is unknown or its entity is in no country of the list (4U1I, Northern Ireland is GB). cty.dat is read
 * on the first lookup, about 0.1 s: from a worker thread */
bool             country_of_call(const char *call, char *cc);

/* Two POSIX TZ strings for the same time all year: the same offsets and rules, whatever their names ("<-03>3" and
 * "BRT3", "<+08>-8" and "SGT-8") */
bool             tz_alike(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
