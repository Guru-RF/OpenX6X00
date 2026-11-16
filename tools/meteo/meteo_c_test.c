/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/meteo.c, what APP > Weather, its notices and the web page's socket command share, with
 *  station_position as src/station.c has it (the Makefile cuts it out of there into build/) and stand-ins for the GPS,
 *  the phone's fix, the QTH locator, the country setting and the GUI's language. The cases:
 *  - where the weather is asked for: the GPS, else the phone's fix (no coarser than 1 km), else the QTH locator's
 *    centre, else the country's place (its capital), else none; a fix 10 minutes old or more no longer counts; a
 *    locator that is not one is passed over; the country's place never written into the locator;
 *  - the helper's arguments: the position to 5 decimals, the country ("-" none), the source, the language ("en");
 *  - the levels' names (the US weather service's severity words, the colours of the others) and colours;
 *  - the place's times as the screen shows them ("Mon 16 15:00"), the day's name in the GUI's language;
 *  - a warning on one line: the service's own words, or one worked out from the forecast in the GUI's words; its time
 *    span, "today" and "tomorrow" for the worked-out ones.
 */

#include "meteo.h"

#include "cJSON.h"
#include "countries.h"
#include "qth/qth.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;

static void check(bool ok, const char *what, const char *v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %s\n", what, v ? v : "");
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    char b[300];

    snprintf(b, sizeof(b), "\"%s\", wanted \"%s\"", got, want);
    check(!strcmp(got, want), what, b);
}

/* ---- stand-ins ---- */

static struct {
    struct {
        char x[16];
    } qth;
} params;

static bool gps_ok, phone_ok;
static int  gps_age, phone_age, phone_acc;

bool gps_last_fix(double *lat, double *lon, int *age) {
    if (!gps_ok) return false;
    *lat = 50.8467;
    *lon = 4.3525;
    *age = gps_age;
    return true;
}

bool gps_phone_fix(double *lat, double *lon, int *age, int *acc) {
    if (!phone_ok) return false;
    *lat = 51.2194;
    *lon = 4.4025;
    *age = phone_age;
    *acc = phone_acc;
    return true;
}

#include "build/station_position.c"

static const country_t  belgium = {"BE", 1, "CET-1CEST,M3.5.0,M10.5.0/3", 50.8333f, 4.3333f};
static const country_t *country;
static const char      *lang = "";

const country_t *country_now(void) {
    return country;
}

const char *country_code(char *cc) {
    snprintf(cc, 3, "%s", country ? country->cc : "");
    return cc;
}

const char *country_name(const country_t *c) {
    return c ? "Belgium" : "";
}

const char *i18n_code(void) {
    return lang;
}

/* Spanish for two words, as a translation would have them */
const char *i18n_tr(const char *s) {
    if (!strcmp(lang, "es")) {
        if (!strcmp(s, "Mon")) return "lun";
        if (!strcmp(s, "gusts up to %d km/h")) return "rachas de hasta %d km/h";
        if (!strcmp(s, "%s: %s, from the forecast")) return "%s: %s, según el pronóstico";
        if (!strcmp(s, "Orange")) return "Naranja";
    }
    return s;
}

/* ---- the cases ---- */

static void reset(void) {
    gps_ok = phone_ok = false;
    gps_age = phone_age = 5;
    phone_acc = 20;
    params.qth.x[0] = 0;
    country = NULL;
}

static void check_pos(const char *want_src, double want_lat, double want_lon, const char *what) {
    double      lat = -99, lon = -99;
    const char *src = "?";
    bool        ok = meteo_position(&lat, &lon, &src);
    char        b[160];

    snprintf(b, sizeof(b), "%s %.4f %.4f (%s), wanted %s %.4f %.4f", ok ? "true" : "false", lat, lon, src, want_src,
             want_lat, want_lon);
    if (!want_src[0])
        check(!ok && !src[0], what, b);
    else
        check(ok && !strcmp(src, want_src) && fabs(lat - want_lat) < 1e-4 && fabs(lon - want_lon) < 1e-4, what, b);
}

static void test_position(void) {
    double jo20ft_lat, jo20ft_lon;

    qth_str_to_pos("JO20ft", &jo20ft_lat, &jo20ft_lon);

    reset();
    check_pos("", 0, 0, "nothing known: no position");
    country = &belgium;
    check_pos("Country", 50.8333, 4.3333, "the country alone: its place");
    check(params.qth.x[0] == 0, "the country's place is never written into the locator", params.qth.x);
    snprintf(params.qth.x, sizeof(params.qth.x), "JO20ft");
    check_pos("Locator", jo20ft_lat, jo20ft_lon, "the locator before the country");
    snprintf(params.qth.x, sizeof(params.qth.x), "JO2");
    check_pos("Country", 50.8333, 4.3333, "a locator of 3 characters: passed over");
    snprintf(params.qth.x, sizeof(params.qth.x), "ZZ99");
    check_pos("Country", 50.8333, 4.3333, "a locator that is not one: passed over");
    snprintf(params.qth.x, sizeof(params.qth.x), "JO20ft");
    phone_ok = true;
    check_pos("Phone", 51.2194, 4.4025, "the phone's fix before the locator");
    phone_acc = 1500;
    check_pos("Locator", jo20ft_lat, jo20ft_lon, "a phone placed by its cell (1.5 km): the locator");
    phone_acc = 20;
    phone_age = 600;
    check_pos("Locator", jo20ft_lat, jo20ft_lon, "a phone's fix 10 minutes old: the locator");
    phone_age = 5;
    gps_ok = true;
    check_pos("GPS", 50.8467, 4.3525, "the GPS before the phone");
    gps_age = 700;
    check_pos("Phone", 51.2194, 4.4025, "a GPS fix 10 minutes old: the phone");
    country = NULL;
    params.qth.x[0] = 0;
    phone_ok = false;
    check_pos("", 0, 0, "only an old GPS fix: none");
}

static void test_args(void) {
    char b[160];

    reset();
    meteo_args(b, sizeof(b), 50.8467, 4.3525, "GPS");
    check_str(b, "50.84670 4.35250 - GPS en", "arguments without a country, in English");
    country = &belgium;
    lang = "nl";
    meteo_args(b, sizeof(b), -34.6037, -58.3816, "Country");
    check_str(b, "-34.60370 -58.38160 BE Country nl", "arguments with the country and the language");
    meteo_args(b, sizeof(b), 0, 0, "");
    check_str(b, "0.00000 0.00000 BE - nl", "no source: \"-\"");
    lang = "";
}

static void test_levels(void) {
    check_str(meteo_level_name(2, "MeteoAlarm"), "Yellow", "MeteoAlarm 2");
    check_str(meteo_level_name(3, "SMN"), "Orange", "SMN 3");
    check_str(meteo_level_name(4, "forecast"), "Red", "worked out 4");
    check_str(meteo_level_name(3, "NWS"), "Severe", "the NWS's severity words");
    check_str(meteo_level_name(4, "NWS"), "Extreme", "NWS 4");
    check_str(meteo_level_name(9, "INMET"), "Notice", "a level out of range");
    check(meteo_level_color(2) == 0xffd800 && meteo_level_color(3) == 0xff8c00 && meteo_level_color(4) == 0xe00000,
          "the levels' colours", NULL);
    check(meteo_level_color(1) == 0 && meteo_level_color(0) == 0, "no colour for minor and none", NULL);
}

static void test_times(void) {
    char b[64];

    meteo_time(b, sizeof(b), "2026-11-16 15:00");
    check_str(b, "Mon 16 15:00", "a Monday");
    meteo_time(b, sizeof(b), "2026-03-01 07:05");
    check_str(b, "Sun 1 07:05", "a Sunday, one digit");
    meteo_time(b, sizeof(b), "2028-02-29 23:59");
    check_str(b, "Tue 29 23:59", "a leap day");
    meteo_time(b, sizeof(b), "2026-11-16");
    check_str(b, "", "a date without a time");
    meteo_time(b, sizeof(b), "");
    check_str(b, "", "nothing");
    meteo_time(b, sizeof(b), "16/11/2026 15:00");
    check_str(b, "", "another form");
    check_str(meteo_weekday("2026-11-20"), "Fri", "a Friday");
    lang = "es";
    meteo_time(b, sizeof(b), "2026-11-16 15:00");
    check_str(b, "lun 16 15:00", "the day's name in the GUI's language");
    lang = "";
}

static cJSON *warning(const char *json) {
    cJSON *j = cJSON_Parse(json);

    check(j != NULL, "a warning's JSON", json);
    return j;
}

static void test_warnings(void) {
    char   b[200];
    cJSON *official = warning("{\"src\":\"MeteoAlarm\",\"kind\":\"official\",\"level\":2,\"event\":\"Yellow wind warning\","
                              "\"from\":\"2026-11-16 15:00\",\"to\":\"2026-11-17 09:00\",\"mine\":true}");
    cJSON *nws      = warning("{\"src\":\"NWS\",\"level\":3,\"event\":\"Winter Storm Warning\",\"from\":\"\","
                              "\"to\":\"2026-11-17 11:00\"}");
    cJSON *calc     = warning("{\"src\":\"forecast\",\"kind\":\"computed\",\"level\":3,\"event\":\"Gusts up to 95 km/h\","
                              "\"calc\":\"gust\",\"value\":95,\"day\":1,\"from\":\"2026-11-17 00:00\","
                              "\"to\":\"2026-11-17 23:59\"}");
    cJSON *heat     = warning("{\"src\":\"forecast\",\"level\":2,\"calc\":\"heat\",\"value\":37,\"day\":0}");

    meteo_warning_title(b, sizeof(b), official);
    check_str(b, "Yellow: Yellow wind warning", "an official warning: its own words");
    meteo_warning_when(b, sizeof(b), official, "CET");
    check_str(b, "Mon 16 15:00 to Tue 17 09:00 CET", "its time with the zone");
    meteo_warning_when(b, sizeof(b), official, "");
    check_str(b, "Mon 16 15:00 to Tue 17 09:00", "no zone: no space at the end");
    meteo_warning_title(b, sizeof(b), nws);
    check_str(b, "Severe: Winter Storm Warning", "the NWS's: its severity");
    meteo_warning_when(b, sizeof(b), nws, "MST");
    check_str(b, "until Tue 17 11:00 MST", "only an end");
    meteo_warning_title(b, sizeof(b), calc);
    check_str(b, "Orange: gusts up to 95 km/h, from the forecast", "worked out: the GUI's words");
    meteo_warning_when(b, sizeof(b), calc, "CET");
    check_str(b, "tomorrow", "worked out for tomorrow");
    meteo_warning_title(b, sizeof(b), heat);
    check_str(b, "Yellow: heat, a high of 37 °C, from the forecast", "heat");
    meteo_warning_when(b, sizeof(b), heat, "CET");
    check_str(b, "today", "worked out for today");
    lang = "es";
    meteo_warning_title(b, sizeof(b), calc);
    check_str(b, "Naranja: rachas de hasta 95 km/h, según el pronóstico", "worked out, in Spanish");
    lang = "";
    cJSON_Delete(official);
    cJSON_Delete(nws);
    cJSON_Delete(calc);
    cJSON_Delete(heat);
}

int main(void) {
    test_position();
    test_args();
    test_levels();
    test_times();
    test_warnings();
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "ok", fails, checks);
    return fails ? 1 : 0;
}
