/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax Auto Plan, see plan.h
 *
 *  From OlivLoze's fork (F4OLI).
 */

#include "plan.h"
#include "plan_default.h"

#include "../board.h"
#include "../cJSON.h"
#include "../i18n.h"        /* N_(): the chart words are translated where they show */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAN_FILE   BOARD_DATA "/wefax/plan.json"
#define DAY_S       86400

static wefax_slot_t     slots[WEFAX_PLAN_MAX];
static int              slots_n;
static bool             from_file;
static wefax_bulletin_t kinds[WEFAX_BULLETIN_MAX];
static int              kinds_n;

static int by_start(const void *a, const void *b)
{
    return (int)((const wefax_slot_t *)a)->min - (int)((const wefax_slot_t *)b)->min;
}

/* "bulletin": [{"name": "Analysis", "match": ["NW-surface-analysis", ...]}, ...] */
static int parse_bulletin(cJSON *json, wefax_bulletin_t *out)
{
    cJSON *list = cJSON_GetObjectItem(json, "bulletin"), *it;
    int    n    = 0;

    if (!cJSON_IsArray(list))
        return 0;
    cJSON_ArrayForEach(it, list) {
        cJSON *name = cJSON_GetObjectItem(it, "name"), *match = cJSON_GetObjectItem(it, "match"), *m;

        if (n == WEFAX_BULLETIN_MAX || !cJSON_IsString(name) || !cJSON_IsArray(match))
            continue;

        wefax_bulletin_t *b = &out[n];

        memset(b, 0, sizeof(*b));
        snprintf(b->name, sizeof(b->name), "%s", name->valuestring);
        cJSON_ArrayForEach(m, match)
            if (b->n < WEFAX_BULLETIN_MATCH && cJSON_IsString(m) && m->valuestring[0])
                snprintf(b->match[b->n++], sizeof(b->match[0]), "%s", m->valuestring);
        if (b->n)
            n++;
    }
    return n;
}

/* {"slots": [{"t": "HH:MM", "st", "hz", "prod", "dur", "fill"}, ...], "bulletin": [...]}; a slot with a time or a
 * frequency out of range is left out */
static int parse(const char *text, wefax_slot_t *out, wefax_bulletin_t *kinds_out, int *kinds_got)
{
    cJSON *json = cJSON_Parse(text);
    cJSON *list = json ? cJSON_GetObjectItem(json, "slots") : NULL;
    int    n    = 0;

    if (!cJSON_IsArray(list)) {
        cJSON_Delete(json);
        return -1;
    }

    cJSON *it;

    cJSON_ArrayForEach(it, list) {
        cJSON *t = cJSON_GetObjectItem(it, "t"), *st = cJSON_GetObjectItem(it, "st");
        cJSON *hz = cJSON_GetObjectItem(it, "hz"), *prod = cJSON_GetObjectItem(it, "prod");
        cJSON *dur = cJSON_GetObjectItem(it, "dur"), *fill = cJSON_GetObjectItem(it, "fill");
        int    h, m;

        if (n == WEFAX_PLAN_MAX || !cJSON_IsString(t) || sscanf(t->valuestring, "%d:%d", &h, &m) != 2 || h < 0 ||
            h > 23 || m < 0 || m > 59 || !cJSON_IsNumber(hz) || hz->valuedouble < 100000 || hz->valuedouble > 30000000)
            continue;

        wefax_slot_t *s = &out[n++];

        memset(s, 0, sizeof(*s));
        s->min     = (uint16_t)(h * 60 + m);
        s->freq_hz = (int32_t)hz->valuedouble;
        s->dur     = cJSON_IsNumber(dur) && dur->valuedouble >= 60 && dur->valuedouble <= 3600
                         ? (uint16_t)dur->valuedouble
                         : WEFAX_PLAN_DUR_S;
        s->fill    = cJSON_IsTrue(fill);
        snprintf(s->station, sizeof(s->station), "%s", cJSON_IsString(st) ? st->valuestring : "");
        snprintf(s->product, sizeof(s->product), "%s", cJSON_IsString(prod) ? prod->valuestring : "");
    }
    *kinds_got = parse_bulletin(json, kinds_out);
    cJSON_Delete(json);
    qsort(out, n, sizeof(out[0]), by_start);
    return n;
}

int wefax_plan_parse(const char *json)
{
    int n = parse(json, slots, kinds, &kinds_n);

    slots_n   = n > 0 ? n : 0;
    from_file = false;
    if (n < 0)
        kinds_n = 0;
    return n;
}

int wefax_plan_load(void)
{
    FILE *f = fopen(PLAN_FILE, "r");

    from_file = false;
    if (f) {
        char *text = NULL;
        long  size;

        if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) > 0 && size < 256 * 1024 && !fseek(f, 0, SEEK_SET) &&
            (text = calloc(1, size + 1)) && fread(text, 1, size, f) == (size_t)size) {
            static wefax_slot_t     got[WEFAX_PLAN_MAX];
            static wefax_bulletin_t got_kinds[WEFAX_BULLETIN_MAX];
            int                     got_kinds_n;
            int                     n = parse(text, got, got_kinds, &got_kinds_n);

            /* a file with no slot to go by leaves the firmware's plan */
            if (n > 0) {
                memcpy(slots, got, sizeof(slots[0]) * n);
                memcpy(kinds, got_kinds, sizeof(kinds[0]) * got_kinds_n);
                slots_n   = n;
                kinds_n   = got_kinds_n;
                from_file = true;
            } else {
                printf("WEFAX PLAN: %s is not a plan, the firmware's is used\n", PLAN_FILE);
            }
        }
        free(text);
        fclose(f);
    }
    if (!from_file) {
        slots_n = parse(plan_default_json, slots, kinds, &kinds_n);
        if (slots_n < 0)
            slots_n = kinds_n = 0;
    }
    printf("WEFAX PLAN: %d slots, %d kinds in the Bulletin (%s)\n", slots_n, kinds_n,
           from_file ? PLAN_FILE : "firmware");
    return slots_n;
}

int wefax_plan_count(void)
{
    return slots_n;
}

const wefax_slot_t *wefax_plan_slot(int i)
{
    return i >= 0 && i < slots_n ? &slots[i] : NULL;
}

bool wefax_plan_from_file(void)
{
    return from_file;
}

bool wefax_plan_product_unique(const char *product)
{
    return !product || !product[0] || strstr(product, "unknown");
}

/* ---- times ---- */

int wefax_plan_sod(time_t t)
{
    struct tm tm;

    gmtime_r(&t, &tm);
    return tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

/* d wrapped into -DAY_S/2 .. DAY_S/2 - 1 */
static int half_day(int d)
{
    return ((d % DAY_S) + DAY_S + DAY_S / 2) % DAY_S - DAY_S / 2;
}

int wefax_plan_since(const wefax_slot_t *s, int sod)
{
    return half_day(sod - s->min * 60);
}

int wefax_plan_next(int sod, time_t now, const time_t *done, int *wait)
{
    int next = -1, best = 0;

    for (int i = 0; i < slots_n; i++) {
        int d = -wefax_plan_since(&slots[i], sod) - WEFAX_PLAN_EARLY_S;     /* until its tuning */

        if (d < -WEFAX_PLAN_LATE_S || (done && done[i] && now - done[i] < WEFAX_PLAN_AGAIN_S))
            continue;
        if (next < 0 || d < best) {
            next = i;
            best = d;
        }
    }
    if (wait)
        *wait = best;
    return next;
}

int wefax_plan_slot_of_fax(int32_t freq_hz, int sod)
{
    int best = -1, best_d = 0;

    for (int i = 0; i < slots_n; i++) {
        int d = wefax_plan_since(&slots[i], sod);

        if (abs(slots[i].freq_hz - freq_hz) > 50 || d < -60 || d > 180)
            continue;
        if (best < 0 || abs(d) < best_d) {
            best   = i;
            best_d = abs(d);
        }
    }
    return best;
}

int wefax_plan_last_started(int sod, int *since)
{
    int last = -1, best = 0;

    for (int i = 0; i < slots_n; i++) {
        int d = (sod - slots[i].min * 60 + DAY_S) % DAY_S;      /* 0..DAY_S-1: the day before too */

        if (last < 0 || d < best) {
            last = i;
            best = d;
        }
    }
    if (since)
        *since = best;
    return last;
}

bool wefax_plan_worth_joining(const wefax_slot_t *s, int since)
{
    return s && since >= 0 && since < s->dur - WEFAX_PLAN_JOIN_LEFT_S;
}

bool wefax_plan_keeps_radio(const wefax_slot_t *cur, int elapsed, const wefax_slot_t *next)
{
    int  dur      = cur ? cur->dur : WEFAX_PLAN_DUR_S;
    bool cur_fill = !cur || cur->fill;

    if (elapsed >= dur - WEFAX_PLAN_END_S)
        return false;               /* over by now, its stop tone missed */
    return next->fill || !cur_fill;
}

/* ---- names ---- */

void wefax_plan_file_name(char *out, size_t size, const struct tm *tm, const char *product, int q)
{
    bool named = product && product[0];

    snprintf(out, size, "wefax-%04d%02d%02d-%02d%02d%02d%s%s-q%d.png", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, named ? "-" : "", named ? product : "", q);
}

time_t wefax_plan_name_time(const char *name)
{
    struct tm tm = {0};

    if (strncmp(name, "wefax-", 6) || sscanf(name + 6, "%4d%2d%2d-%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                                             &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    return mktime(&tm);     /* the names are in local time */
}

/* Where "-q<mark>.png" starts, NULL for none */
static const char *mark_at(const char *name)
{
    const char *q = strrchr(name, '-');
    char       *end;

    if (!q || q[1] != 'q')
        return NULL;
    strtol(q + 2, &end, 10);
    return end != q + 2 && !strcmp(end, ".png") ? q : NULL;
}

int wefax_plan_name_mark(const char *name)
{
    const char *q = mark_at(name);

    return q ? atoi(q + 2) : -1;
}

void wefax_plan_name_product(const char *name, char *out, size_t size)
{
    const char *q = mark_at(name);
    ptrdiff_t   len;

    out[0] = '\0';
    if (strlen(name) < 22 || name[21] != '-' || !q)
        return;     /* wefax-YYYYMMDD-HHMMSS.png, one from before the marks: none */
    len = q - (name + 22);
    if (len > 0)
        snprintf(out, size, "%.*s", (int)len, name + 22);
}

const wefax_slot_t *wefax_plan_slot_of_file(const char *name)
{
    char                chart[48];
    time_t              t = wefax_plan_name_time(name);
    const wefax_slot_t *best = NULL;
    int                 best_d = 0;

    wefax_plan_name_product(name, chart, sizeof(chart));
    if (!chart[0] || t == (time_t)-1)
        return NULL;

    int sod = wefax_plan_sod(t);

    for (int i = 0; i < slots_n; i++) {
        int d = (sod - slots[i].min * 60 + DAY_S) % DAY_S;      /* saved this long after the slot's start */

        if (!strcmp(slots[i].product, chart) && (!best || d < best_d)) {
            best   = &slots[i];
            best_d = d;
        }
    }
    return best;
}

int wefax_plan_copies(const char *dir, const char *product, time_t when, const char *keep, bool drop,
                      void (*dropped)(const char *dir, const char *name))
{
    DIR *d    = opendir(dir);
    int  best = -1;

    if (!d)
        return -1;

    struct dirent *de;

    while ((de = readdir(d))) {
        char   chart[48];
        int    q = wefax_plan_name_mark(de->d_name);
        time_t t = wefax_plan_name_time(de->d_name);

        wefax_plan_name_product(de->d_name, chart, sizeof(chart));
        if (q < 0 || t == (time_t)-1 || strcmp(chart, product) || llabs((long long)(t - when)) > WEFAX_PLAN_SAME_S ||
            (keep && !strcmp(de->d_name, keep)))
            continue;
        if (drop) {
            char path[512];

            snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
            if (remove(path) == 0) {
                printf("WEFAX PLAN: %s dropped, a better copy came in\n", de->d_name);
                if (dropped)
                    dropped(dir, de->d_name);
            }
        } else if (q > best) {
            best = q;
        }
    }
    closedir(d);
    return best;
}

/* ---- the Bulletin ---- */

int wefax_plan_bulletin_count(void)
{
    return kinds_n;
}

const wefax_bulletin_t *wefax_plan_bulletin(int i)
{
    return i >= 0 && i < kinds_n ? &kinds[i] : NULL;
}

bool wefax_bulletin_has(const wefax_bulletin_t *b, const char *product)
{
    for (int i = 0; b && product && i < b->n; i++)
        if (!strncmp(product, b->match[i], strlen(b->match[i])))
            return true;
    return false;
}

int wefax_plan_next_of_kind(const wefax_bulletin_t *b, int sod, int *wait)
{
    int next, w = 0;

    if (!b || !wefax_plan_coming(b, NULL, sod, &next, &w, 1))
        next = -1, w = 0;
    if (wait)
        *wait = w;
    return next;
}

int wefax_plan_coming(const wefax_bulletin_t *b, const char *product, int sod, int *slot, int *wait, int max)
{
    int n = 0;

    for (int i = 0; max > 0 && i < slots_n; i++) {
        if (b ? !wefax_bulletin_has(b, slots[i].product) : product && strcmp(product, slots[i].product))
            continue;

        int d  = ((slots[i].min * 60 - sod) % DAY_S + DAY_S) % DAY_S;
        int at = n < max ? n : max - 1;

        if (n == max && d >= wait[max - 1])
            continue;
        while (at > 0 && wait[at - 1] > d) {        /* in order, soonest first */
            wait[at] = wait[at - 1];
            slot[at] = slot[at - 1];
            at--;
        }
        wait[at] = d;
        slot[at] = i;
        if (n < max)
            n++;
    }
    return n;
}

/* ---- the copies ---- */

const char *wefax_plan_quality_word(int q)
{
    return q < 0 ? "" : q >= WEFAX_MARK_GOOD ? N_("good") : q >= WEFAX_MARK_FAIR ? N_("fair") : N_("poor");
}

bool wefax_plan_bulletin_prefers(const char *cand, const char *have)
{
    if (!have || !have[0])
        return true;

    bool cand_fair = wefax_plan_name_mark(cand) >= WEFAX_MARK_FAIR;
    bool have_fair = wefax_plan_name_mark(have) >= WEFAX_MARK_FAIR;

    if (cand_fair != have_fair)
        return cand_fair;
    return strcmp(cand, have) > 0;     /* the names sort by time */
}

/* The hours in a name's "-48h" or "-36-48h" (an "h" closing the part, so not "-850hPa"); "" none */
static void hours_of(const char *p, char *out, size_t size)
{
    out[0] = '\0';
    for (const char *s = p; *s; s++) {
        if (*s != '-' || !isdigit((unsigned char)s[1]))
            continue;

        const char *e = s + 1;

        while (isdigit((unsigned char)*e) || (*e == '-' && isdigit((unsigned char)e[1])))
            e++;
        if (*e == 'h' && (e[1] == '-' || !e[1])) {
            snprintf(out, size, "%.*s h", (int)(e - (s + 1)), s + 1);
            return;
        }
    }
}

const char *wefax_plan_chart_kind(const char *product, char *hours, size_t size)
{
    const char *p = product ? product : "";

    hours_of(p, hours, size);
    if (!p[0] || strstr(p, "unknown")) {
        hours[0] = '\0';
        return N_("Unsorted");
    }
    if (strstr(p, "sigwx"))
        return N_("Significant weather");
    if (strstr(p, "surface-analysis-Atlantic"))
        return N_("Analysis, North Atlantic");
    if (strstr(p, "analysis"))
        return N_("Analysis");
    if (strstr(p, "hPa"))
        return N_("Upper air");
    if (strstr(p, "forecast"))
        return N_("Forecast");
    if (strstr(p, "sea-swell") || strstr(p, "waves"))
        return N_("Sea and swell");
    if (strstr(p, "wind"))
        return N_("Wind");
    if (strstr(p, "rain-visibility"))
        return N_("Rain and visibility");
    if (strstr(p, "-ice-") || !strncmp(p, "ice-", 4)) {
        hours[0] = '\0';
        return N_("Ice");
    }
    hours[0] = '\0';
    return NULL;
}
