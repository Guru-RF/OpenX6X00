/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Weather: what APP > Weather (dialog_meteo.c), its notices and the web page's socket command share:
 *  where the weather is asked for, the helper's arguments, the warnings' level names and the times shown.
 *  The helper is rootfs/usr/sbin/x6100-meteo (its header has the services and their terms).
 *
 *  After the CX8ANM edition of OpenX6X00 (condoruy), its Meteo app.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef X6100_SBIN
#define X6100_SBIN "/usr/sbin"
#endif

#define METEO_HELPER      X6100_SBIN "/x6100-meteo"
#define METEO_FILE        "/tmp/x6100-meteo.json"          /* the app's answer, the web page's too */
#ifndef METEO_ALERTS_FILE                                  /* tools/meteo: its own */
#define METEO_ALERTS_FILE "/tmp/x6100-meteo-alerts.json"   /* the notices' check */
#endif

/* Settings > General > STATION > Weather from (cfg_meteo_from): where the radio is (its GPS, the phone's, the QTH
 * locator), the QTH locator whatever the GPS says (the weather at home while the radio is out), or the place typed in
 * Weather place (cfg_meteo_place) */
enum { METEO_FROM_RADIO, METEO_FROM_LOCATOR, METEO_FROM_TYPED };

#define METEO_PLACE_MAX 41      /* Weather place's text, its end included */

/* Settings > General > STATION > Weather units (cfg_meteo_units, #315): metric, as the services give every number, or
 * the US units. Only what is shown changes: the helper's answer and the warnings' thresholds stay metric. The web
 * Weather page (views/meteo.html, wx()) shows them by the same rules */
enum { METEO_UNITS_METRIC, METEO_UNITS_US };

/* A number of the weather, by its metric unit */
typedef enum {
    METEO_TEMP,     /* °C, in US units °F */
    METEO_SPEED,    /* km/h, mph */
    METEO_PRESS,    /* hPa, inHg */
    METEO_RAIN,     /* mm, in */
    METEO_DIST,     /* km, mi */
} meteo_qty_t;

/* What follows the number: its unit after a space ("12.3 °C"), a degree sign alone ("12°"), nothing */
typedef enum { METEO_UNIT, METEO_DEG, METEO_BARE } meteo_show_t;

/* Weather units is US */
bool meteo_us(void);

/* v, metric, in the units shown and rounded as there: dec the decimals in metric (as each screen asks); in US units
 * as a US forecast has them, whole degrees and mph, inHg and inches to the hundredth, miles to the tenth under 10.
 * *shown_dec: those decimals (NULL: not wanted). Never -0 */
double meteo_value(meteo_qty_t q, double v, int dec, int *shown_dec);

/* Its unit in the units shown: "°C", "km/h", "hPa", "mm", "km"; "°F", "mph", "inHg", "in", "mi" */
const char *meteo_unit(meteo_qty_t q);

/* v, metric, as shown: "12.3 °C" or "54 °F", "29.92 inHg", "12°", "40" (show) */
void meteo_format(char *out, size_t size, meteo_qty_t q, double v, int dec, meteo_show_t show);

/* Where the weather is asked for, by Weather from: where the radio is (station_position: its GPS, the phone's, the QTH
 * locator), the QTH locator alone, or the place typed; else the place of the country set in Settings > General >
 * STATION (its capital or largest city, as the time zone database has it): the locator is never set from it. A place
 * chosen but not typed counts as where the radio is. src: "GPS", "Phone", "Locator", "Typed", "Country". False
 * without any. LVGL thread */
bool meteo_position(double *lat, double *lon, const char **src);

/* A place typed for the weather: two numbers, degrees north and east ("50.85, 4.35", "-34.9 -56.16"), a letter N, S,
 * E or W after a number giving its side ("34.9S 56.16W"); or a locator of 4, 6 or 8 characters (JO20ft), its centre.
 * False for anything else, a latitude beyond 90 or a longitude beyond 180 */
bool meteo_place_parse(const char *s, double *lat, double *lon);

/* The helper's arguments after its command for that position: "LAT LON CC SRC LANG", the country set ("-" none) and
 * the GUI's language ("en" English) */
void meteo_args(char *buf, size_t size, double lat, double lon, const char *src);

/* A warning's level (1 minor .. 4 red) as the service has it, in English for _(): the US weather service's severity
 * words ("Moderate"), the colours of the others and of the warnings worked out from the forecast ("Yellow") */
const char *meteo_level_name(int level, const char *src);

/* A colour for the level: yellow, orange, red (0xRRGGBB); 0 for none (minor) */
unsigned meteo_level_color(int level);

/* "2026-11-16 15:00" (the place's time, as the helper writes it) as "Mon 16 15:00", the day's name translated;
 * "" for a time not of that form */
void meteo_time(char *out, size_t size, const char *local);

/* The same day's name: "Mon" for "2026-11-16", translated; "" for no date */
const char *meteo_weekday(const char *date);

struct cJSON;

/* A warning of the helper's answer on one line, its level's name first: "Yellow: Yellow wind warning" (the service's
 * own words), "Orange: gusts up to 95 km/h, from the forecast" (one worked out here, in the GUI's language and the
 * units of Weather units: "gusts up to 59 mph") */
void meteo_warning_title(char *out, size_t size, const struct cJSON *a);

/* Its time in the place's: "Mon 16 15:00 to Tue 17 09:00 CET" (tz: the zone's abbreviation), "today" or "tomorrow"
 * for one worked out from the forecast; "" without one */
void meteo_warning_when(char *out, size_t size, const struct cJSON *a, const char *tz);

/* A warning worth a notice (meteo_alerts.c): the place's own, not over at now, from the country's service yellow and
 * up, worked out from the forecast orange and up. until: when it is over (its end, else now and 3 days) */
bool meteo_alert_worth(const struct cJSON *a, time_t now, time_t *until);

#ifdef __cplusplus
}
#endif
