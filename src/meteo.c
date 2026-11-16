/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Weather: the parts the app, its notices and the web page's socket command share (meteo.h)
 *
 *  After the CX8ANM edition of OpenX6X00 (condoruy), its Meteo app.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "meteo.h"

#include "cJSON.h"
#include "countries.h"
#include "i18n.h"
#include "station.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

bool meteo_position(double *lat, double *lon, const char **src) {
    const country_t *c;

    if (station_position(lat, lon, src)) return true;
    c = country_now();
    if (c && (c->lat != 0 || c->lon != 0)) {
        *lat = c->lat;
        *lon = c->lon;
        *src = "Country";
        return true;
    }
    *src = "";
    return false;
}

void meteo_args(char *buf, size_t size, double lat, double lon, const char *src) {
    char        cc[3];
    const char *lang = i18n_code();

    country_code(cc);
    snprintf(buf, size, "%.5f %.5f %s %s %s", lat, lon, cc[0] ? cc : "-", src && src[0] ? src : "-",
             lang && lang[0] ? lang : "en");
}

const char *meteo_level_name(int level, const char *src) {
    static const char *const colours[]  = {N_("Notice"), N_("Minor"), N_("Yellow"), N_("Orange"), N_("Red")};
    static const char *const severity[] = {N_("Notice"), N_("Minor"), N_("Moderate"), N_("Severe"), N_("Extreme")};

    if (level < 0 || level > 4) level = 0;
    return src && !strcmp(src, "NWS") ? severity[level] : colours[level];
}

unsigned meteo_level_color(int level) {
    switch (level) {
        case 2:  return 0xffd800;
        case 3:  return 0xff8c00;
        case 4:  return 0xe00000;
        default: return 0;
    }
}

/* The date of "YYYY-MM-DD..." as a struct tm with its day of the week; false when it is not one */
static bool date_of(const char *s, struct tm *tm) {
    int y, m, d;

    if (!s || sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    memset(tm, 0, sizeof(*tm));
    tm->tm_year = y - 1900;
    tm->tm_mon  = m - 1;
    tm->tm_mday = d;
    tm->tm_hour = 12;
    time_t t = timegm(tm);  /* fills in tm_wday */

    return t != (time_t)-1;
}

const char *meteo_weekday(const char *date) {
    static const char *const days[] = {N_("Sun"), N_("Mon"), N_("Tue"), N_("Wed"), N_("Thu"), N_("Fri"), N_("Sat")};
    struct tm tm;

    return date_of(date, &tm) ? _(days[tm.tm_wday]) : "";
}

void meteo_time(char *out, size_t size, const char *local) {
    struct tm tm;
    int       h, mi;

    if (!date_of(local, &tm) || strlen(local) < 16 || sscanf(local + 11, "%2d:%2d", &h, &mi) != 2) {
        if (size) out[0] = 0;
        return;
    }
    snprintf(out, size, "%s %d %02d:%02d", meteo_weekday(local), tm.tm_mday, h, mi);
}

static const char *jstr(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));

    return s ? s : "";
}

static int jint(const cJSON *o, const char *k, int def) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsNumber(j) ? (int)lround(j->valuedouble) : def;
}

void meteo_warning_title(char *out, size_t size, const cJSON *a) {
    const char *calc = jstr(a, "calc"), *level = _(meteo_level_name(jint(a, "level", 0), jstr(a, "src")));
    int         v    = jint(a, "value", 0);
    char        what[96];

    if (!calc[0]) {
        snprintf(out, size, "%s: %s", level, jstr(a, "event"));
        return;
    }
    /* Worked out from Open-Meteo's forecast (x6100-meteo computed_alerts): the GUI's own words */
    if (!strcmp(calc, "gust"))
        snprintf(what, sizeof(what), _("gusts up to %d km/h"), v);
    else if (!strcmp(calc, "rain"))
        snprintf(what, sizeof(what), _("%d mm of rain"), v);
    else if (!strcmp(calc, "storm"))
        snprintf(what, sizeof(what), "%s", _("thunderstorms"));
    else if (!strcmp(calc, "hail"))
        snprintf(what, sizeof(what), "%s", _("thunderstorms with hail"));
    else if (!strcmp(calc, "heat"))
        snprintf(what, sizeof(what), _("heat, a high of %d °C"), v);
    else if (!strcmp(calc, "cold"))
        snprintf(what, sizeof(what), _("cold, a low of %d °C"), v);
    else if (!strcmp(calc, "uv"))
        snprintf(what, sizeof(what), _("extreme UV, index %d"), v);
    else
        snprintf(what, sizeof(what), "%s", jstr(a, "event"));
    snprintf(out, size, _("%s: %s, from the forecast"), level, what);
}

void meteo_warning_when(char *out, size_t size, const cJSON *a, const char *tz) {
    char from[32], to[32];

    if (jstr(a, "calc")[0]) {
        snprintf(out, size, "%s", jint(a, "day", 0) == 0 ? _("today") : _("tomorrow"));
        return;
    }
    meteo_time(from, sizeof(from), jstr(a, "from"));
    meteo_time(to, sizeof(to), jstr(a, "to"));
    if (from[0] && to[0])
        snprintf(out, size, _("%s to %s %s"), from, to, tz ? tz : "");
    else if (to[0])
        snprintf(out, size, _("until %s %s"), to, tz ? tz : "");
    else if (from[0])
        snprintf(out, size, _("from %s %s"), from, tz ? tz : "");
    else if (size)
        out[0] = 0;
    /* no zone: no space at the end */
    size_t l = strlen(out);

    while (l && out[l - 1] == ' ') out[--l] = 0;
}

bool meteo_alert_worth(const cJSON *a, time_t now, time_t *until) {
    const cJSON *end = cJSON_GetObjectItemCaseSensitive(a, "end");

    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "mine")) || !jstr(a, "id")[0]) return false;
    if (jint(a, "level", 0) < (!strcmp(jstr(a, "kind"), "computed") ? 3 : 2)) return false;
    *until = cJSON_IsNumber(end) && end->valuedouble > 0 ? (time_t)end->valuedouble : now + 3 * 86400;
    return *until > now;
}
