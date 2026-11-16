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

/* Where the weather is asked for: where the radio is (station_position: its GPS, the phone's, the QTH locator), else
 * the place of the country set in Settings > General > STATION (its capital or largest city, as the time zone
 * database has it): the locator is never set from it. src: "GPS", "Phone", "Locator", "Country". False without any.
 * LVGL thread */
bool meteo_position(double *lat, double *lon, const char **src);

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
 * own words), "Orange: gusts up to 95 km/h, from the forecast" (one worked out here, in the GUI's language) */
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
