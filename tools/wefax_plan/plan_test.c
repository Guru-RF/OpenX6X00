/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of WeFax Auto Plan (src/wefax/plan.c) and the quality mark (src/wefax/quality.c), see the Makefile.
 *  Run with TZ=UTC: the names are in local time, as the radio writes them.
 */

#include "wefax/night.h"
#include "wefax/plan.h"
#include "wefax/quality.h"
#include "wefax/thumb.h"
#include "wefax/tilt.h"

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WIDTH   1809        /* WEFAX_WIDTH */

static int fails, checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            fails++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static int hms(int h, int m, int s)
{
    return h * 3600 + m * 60 + s;
}

/* The slot starting at h:m, -1 for none */
static int slot_at(int h, int m)
{
    for (int i = 0; i < wefax_plan_count(); i++)
        if (wefax_plan_slot(i)->min == h * 60 + m)
            return i;
    return -1;
}

static const char *product_of(int i)
{
    const wefax_slot_t *s = wefax_plan_slot(i);

    return s ? s->product : "(none)";
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* ---- the plan ---- */

static void test_load(void)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/wefax/plan.json", BOARD_DATA);
    unlink(path);

    int n = wefax_plan_load();

    CHECK(n == 82 && !wefax_plan_from_file(), "the firmware's plan: %d slots", n);
    CHECK(wefax_plan_bulletin_count() == 9, "its Bulletin: %d kinds", wefax_plan_bulletin_count());
    for (int i = 1; i < n; i++)
        CHECK(wefax_plan_slot(i - 1)->min <= wefax_plan_slot(i)->min, "sorted at %d", i);

    /* a plan on the card replaces it; a slot out of range is left out */
    write_file(path, "{\"slots\": [{\"t\": \"12:00\", \"st\": \"Here\", \"hz\": 7880000, \"prod\": \"X-a\"},"
                     "{\"t\": \"25:00\", \"st\": \"Bad\", \"hz\": 7880000, \"prod\": \"X-b\"},"
                     "{\"t\": \"06:30\", \"st\": \"Here\", \"hz\": 50000, \"prod\": \"X-c\"},"
                     "{\"t\": \"06:00\", \"st\": \"Here\", \"hz\": 3855000, \"prod\": \"X-d\", \"dur\": 1200, "
                     "\"fill\": true}]}");
    n = wefax_plan_load();
    CHECK(n == 2 && wefax_plan_from_file(), "the card's plan: %d slots", n);
    CHECK(n == 2 && wefax_plan_slot(0)->min == 360 && wefax_plan_slot(0)->fill && wefax_plan_slot(0)->dur == 1200 &&
              wefax_plan_slot(1)->dur == WEFAX_PLAN_DUR_S && !wefax_plan_slot(1)->fill,
          "the card's slots read");
    CHECK(wefax_plan_bulletin_count() == 0, "no Bulletin in it");

    /* one that is no plan: the firmware's again */
    write_file(path, "{\"slots\": 3}");
    n = wefax_plan_load();
    CHECK(n == 82 && !wefax_plan_from_file(), "a broken plan.json: the firmware's, %d slots", n);
    unlink(path);
}

/* Which chart comes next, when, on which frequency (the firmware's plan) */
static void test_next(void)
{
    time_t done[WEFAX_PLAN_MAX] = {0};
    time_t now                  = 1763290000;   /* any time: done[] counts from it */
    int    wait, i;

    wefax_plan_load();

    /* 04:29:40: DWD's 04:30 analysis on 3855 kHz, tuned in 10 s (10 s before its start) */
    i = wefax_plan_next(hms(4, 29, 40), now, done, &wait);
    CHECK(i == slot_at(4, 30) && wait == 10, "04:29:40 next %s in %d s", product_of(i), wait);
    CHECK(i >= 0 && wefax_plan_slot(i)->freq_hz == 3855000 && !strcmp(wefax_plan_slot(i)->station, "DWD") &&
              !strcmp(wefax_plan_slot(i)->product, "DWD-surface-analysis-00Z"),
          "04:30 is DWD's analysis on 3855 kHz");

    /* 04:29:50: now */
    i = wefax_plan_next(hms(4, 29, 50), now, done, &wait);
    CHECK(i == slot_at(4, 30) && wait == 0, "04:29:50 take %s, wait %d", product_of(i), wait);

    /* a tick missed: 04:31:00 still takes it (70 s late), 04:32 does not */
    i = wefax_plan_next(hms(4, 31, 0), now, done, &wait);
    CHECK(i == slot_at(4, 30) && wait == -70, "04:31 late take %s, wait %d", product_of(i), wait);
    i = wefax_plan_next(hms(4, 32, 0), now, done, &wait);
    CHECK(i == slot_at(5, 0), "04:32: too late, next %s", product_of(i));

    /* taken: not again within the hour, the next one named */
    done[slot_at(4, 30)] = now;
    i = wefax_plan_next(hms(4, 29, 55), now + 5, done, &wait);
    CHECK(i == slot_at(5, 0) && wait == hms(0, 30, 5) - 10, "after 04:30 taken: %s in %d s", product_of(i), wait);
    done[slot_at(4, 30)] = now - WEFAX_PLAN_AGAIN_S;
    i = wefax_plan_next(hms(4, 29, 55), now, done, &wait);
    CHECK(i == slot_at(4, 30), "an hour later it may be taken again");

    /* over midnight: 23:59:55 takes 00:00's chart; 23:50 waits for 23:48's successor... */
    memset(done, 0, sizeof(done));
    i = wefax_plan_next(hms(23, 59, 55), now, done, &wait);
    CHECK(i == slot_at(0, 0) && wait == -5, "23:59:55 take %s, wait %d", product_of(i), wait);
    i = wefax_plan_next(hms(0, 0, 30), now, done, &wait);
    CHECK(i == slot_at(0, 0) && wait == -40, "00:00:30 still %s", product_of(i));

    /* the best frequency by the hour: Northwood on 2618.5 at night, 4610 in the morning, 8040 by day */
    CHECK(wefax_plan_slot(slot_at(2, 0))->freq_hz == 2618500, "Northwood 02:00 on 2618.5 kHz");
    CHECK(wefax_plan_slot(slot_at(7, 12))->freq_hz == 4610000, "Northwood 07:12 on 4610 kHz");
    CHECK(wefax_plan_slot(slot_at(12, 0))->freq_hz == 8040000, "Northwood 12:00 on 8040 kHz");
    CHECK(wefax_plan_slot(slot_at(10, 50))->freq_hz == 7880000, "DWD 10:50 on 7880 kHz");
    CHECK(wefax_plan_slot(slot_at(22, 0))->freq_hz == 3855000, "DWD 22:00 on 3855 kHz");
}

/* Which slot a fax is, a chart going on, a fax against the next slot */
static void test_fax(void)
{
    int since, i;

    wefax_plan_load();

    /* confirmed 70 s after DWD's 10:29 start, on its frequency */
    i = wefax_plan_slot_of_fax(7880000, hms(10, 30, 10));
    CHECK(i == slot_at(10, 29), "a fax at 10:30:10 on 7880: %s", product_of(i));
    CHECK(wefax_plan_slot_of_fax(7880000 + 30, hms(10, 30, 10)) == i, "30 Hz off is the same station");
    CHECK(wefax_plan_slot_of_fax(8040000, hms(10, 30, 10)) < 0, "another frequency: none");
    CHECK(wefax_plan_slot_of_fax(7880000, hms(10, 33, 0)) < 0, "4 min after its start: none");
    CHECK(wefax_plan_slot_of_fax(7880000, hms(10, 28, 30)) == i, "30 s early: that slot");
    i = wefax_plan_slot_of_fax(7880000, hms(10, 50, 50));
    CHECK(i == slot_at(10, 50), "10:50:50: %s", product_of(i));

    /* opened at 05:30: DWD's 05:25 chart (20 min long) is going on, worth joining; at 05:44 no longer */
    i = wefax_plan_last_started(hms(5, 30, 0), &since);
    CHECK(i == slot_at(5, 25) && since == 300, "05:30: %s started %d s ago", product_of(i), since);
    CHECK(wefax_plan_worth_joining(wefax_plan_slot(i), since), "worth joining at 05:30");
    i = wefax_plan_last_started(hms(5, 44, 0), &since);
    CHECK(i == slot_at(5, 25) && !wefax_plan_worth_joining(wefax_plan_slot(i), since), "not at 05:44 (%d s)", since);
    i = wefax_plan_last_started(hms(0, 5, 0), &since);
    CHECK(i == slot_at(0, 0) && since == 300, "00:05: %s %d s", product_of(i), since);
    i = wefax_plan_last_started(hms(3, 59, 0), &since);
    CHECK(i == slot_at(3, 24) && since == 35 * 60, "03:59: the 03:24 chart, %d s", since);

    /* the next slot's time with a fax coming in */
    wefax_slot_t plan = {.dur = 690}, filler = {.dur = 690, .fill = true};

    CHECK(wefax_plan_keeps_radio(&plan, 200, &plan), "a chart of the plan is not cut for another");
    CHECK(wefax_plan_keeps_radio(&plan, 200, &filler), "nor for a filler");
    CHECK(!wefax_plan_keeps_radio(&filler, 200, &plan), "a filler gives way to a chart of the plan");
    CHECK(wefax_plan_keeps_radio(&filler, 200, &filler), "a filler keeps the radio against a filler");
    CHECK(!wefax_plan_keeps_radio(NULL, 200, &plan), "a fax that is none of the plan's gives way");
    CHECK(!wefax_plan_keeps_radio(&plan, 670, &plan), "a chart 20 s from its end gives way (its stop missed)");
}

/* ---- names and the best copy ---- */

static void dropped_cb(const char *dir, const char *name)
{
    (void)dir;
    printf("  dropped %s\n", name);
}

static bool exists(const char *dir, const char *name)
{
    char        path[512];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return stat(path, &st) == 0;
}

static void test_names(void)
{
    struct tm tm = {.tm_year = 126, .tm_mon = 10, .tm_mday = 16, .tm_hour = 4, .tm_min = 41, .tm_sec = 7};
    char      name[160], chart[48];

    wefax_plan_file_name(name, sizeof(name), &tm, "DWD-surface-analysis-00Z", 57);
    CHECK(!strcmp(name, "wefax-20261116-044107-DWD-surface-analysis-00Z-q57.png"), "name %s", name);
    CHECK(wefax_plan_name_mark(name) == 57, "its mark");
    wefax_plan_name_product(name, chart, sizeof(chart));
    CHECK(!strcmp(chart, "DWD-surface-analysis-00Z"), "its chart %s", chart);

    tm.tm_isdst = -1;
    time_t t = mktime(&tm);

    CHECK(wefax_plan_name_time(name) == t, "its time");

    wefax_plan_file_name(name, sizeof(name), &tm, "", 3);
    CHECK(!strcmp(name, "wefax-20261116-044107-q3.png"), "no chart: %s", name);
    wefax_plan_name_product(name, chart, sizeof(chart));
    CHECK(!chart[0] && wefax_plan_name_mark(name) == 3, "no chart, mark 3");

    /* one saved before the marks */
    wefax_plan_name_product("wefax-20261101-120000.png", chart, sizeof(chart));
    CHECK(!chart[0] && wefax_plan_name_mark("wefax-20261101-120000.png") < 0, "an old name: no chart, no mark");
    CHECK(wefax_plan_name_time("other.png") == (time_t)-1, "not ours");

    CHECK(wefax_plan_product_unique("NW-unknown") && wefax_plan_product_unique("") &&
              !wefax_plan_product_unique("NW-surface-analysis-00Z"),
          "unknown charts keep every copy");

    /* the copies of a chart: within 16 h of the new one, other charts and old names left alone */
    char dir[512];

    snprintf(dir, sizeof(dir), "%s/wefax", BOARD_DATA);

    static const char *files[] = {
        "wefax-20261116-034000-NW-surface-analysis-00Z-q40.png",   /* 1 h before: a copy */
        "wefax-20261115-090000-NW-surface-analysis-00Z-q90.png",   /* 19 h before: yesterday's */
        "wefax-20261116-030000-NW-surface-analysis-06Z-q99.png",   /* another chart */
        "wefax-20261116-030500-NW-surface-analysis-00Z.png",       /* before the marks */
        "wefax-20261116-031000-NW-surface-analysis-00Z-q70.png.part",  /* being written */
    };
    for (size_t k = 0; k < sizeof(files) / sizeof(files[0]); k++) {
        char path[600];

        snprintf(path, sizeof(path), "%s/%s", dir, files[k]);
        write_file(path, "x");
    }

    tm.tm_hour = 4;
    tm.tm_min  = 40;
    tm.tm_sec  = 0;
    t          = mktime(&tm);

    int best = wefax_plan_copies(dir, "NW-surface-analysis-00Z", t, NULL, false, NULL);

    CHECK(best == 40, "the best copy so far: %d", best);
    CHECK(wefax_plan_copies(dir, "NW-surface-analysis-12Z", t, NULL, false, NULL) == -1, "none of 12Z");

    /* a better one written: the worse goes, the rest stays */
    wefax_plan_file_name(name, sizeof(name), &tm, "NW-surface-analysis-00Z", 65);

    char path[sizeof(dir) + sizeof(name) + 1];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    write_file(path, "x");
    wefax_plan_copies(dir, "NW-surface-analysis-00Z", t, name, true, dropped_cb);
    CHECK(!exists(dir, files[0]), "the q40 copy dropped");
    CHECK(exists(dir, name) && exists(dir, files[1]) && exists(dir, files[2]) && exists(dir, files[3]) &&
              exists(dir, files[4]),
          "the new one, yesterday's, the other chart, the old name and the one being written kept");
    CHECK(wefax_plan_copies(dir, "NW-surface-analysis-00Z", t, NULL, false, NULL) == 65, "the best now 65");
}

/* The slot a saved chart came from: the last of its chart started before it was saved */
static void test_slot_of_file(void)
{
    wefax_plan_load();

    const wefax_slot_t *s = wefax_plan_slot_of_file("wefax-20261116-044107-DWD-surface-analysis-00Z-q57.png");

    CHECK(s && s->min == 4 * 60 + 30 && s->freq_hz == 3855000, "DWD's 00Z analysis saved 04:41: the 04:30 slot");
    s = wefax_plan_slot_of_file("wefax-20261116-002000-DWD-surface-analysis-00Z-q57.png");
    CHECK(s && s->min == 4 * 60 + 30, "saved 00:20: yesterday's 04:30 is the last one started");
    CHECK(!wefax_plan_slot_of_file("wefax-20261116-044107-q57.png"), "no chart: no slot");
    CHECK(!wefax_plan_slot_of_file("wefax-20261116-044107-XX-nothing-q57.png"), "a chart not in the plan: none");
}

static void test_bulletin(void)
{
    wefax_plan_load();

    const wefax_bulletin_t *analysis = NULL, *ice = NULL, *days = NULL, *sigwx = NULL;

    for (int k = 0; k < wefax_plan_bulletin_count(); k++) {
        const wefax_bulletin_t *b = wefax_plan_bulletin(k);

        if (!strcmp(b->name, "Analysis"))
            analysis = b;
        if (!strcmp(b->name, "Ice"))
            ice = b;
        if (!strcmp(b->name, "Forecast 3-5 days"))
            days = b;
        if (!strcmp(b->name, "Significant weather"))
            sigwx = b;
    }
    CHECK(analysis && ice && days && sigwx, "the kinds Analysis, Ice, Forecast 3-5 days, Significant weather");
    if (!analysis || !ice || !days || !sigwx)
        return;
    CHECK(wefax_bulletin_has(analysis, "NW-surface-analysis-00Z") &&
              wefax_bulletin_has(analysis, "DWD-surface-analysis-06Z") &&
              !wefax_bulletin_has(analysis, "NW-surface-forecast-24h-00Z"),
          "Analysis takes both stations' analyses only");

    /* DWD's 05:25 and 18:00 charts are the North Atlantic surface analysis (their titles), not significant weather */
    CHECK(!strcmp(product_of(slot_at(5, 25)), "DWD-surface-analysis-Atlantic-00Z") &&
              !strcmp(product_of(slot_at(18, 0)), "DWD-surface-analysis-Atlantic-12Z"),
          "05:25 and 18:00: %s, %s", product_of(slot_at(5, 25)), product_of(slot_at(18, 0)));
    CHECK(wefax_bulletin_has(analysis, product_of(slot_at(18, 0))) && !wefax_bulletin_has(sigwx, product_of(slot_at(18, 0))),
          "the Atlantic analysis is an Analysis");
    CHECK(wefax_bulletin_has(sigwx, product_of(slot_at(9, 36))), "Northwood's upper-air SIGWX is Significant weather");

    /* Northwood's slots named from the charts received: 10:00, 10:12, 18:24-18:48 no longer unknown nor fillers */
    CHECK(!strcmp(product_of(slot_at(10, 0)), "NW-surface-analysis-06Z") &&
              !strcmp(product_of(slot_at(10, 12)), "NW-surface-forecast-24h-06Z") &&
              !strcmp(product_of(slot_at(18, 36)), "NW-surface-forecast-24h-12Z"),
          "10:00 %s, 10:12 %s, 18:36 %s", product_of(slot_at(10, 0)), product_of(slot_at(10, 12)),
          product_of(slot_at(18, 36)));
    CHECK(!strcmp(product_of(slot_at(1, 48)), "NW-sea-swell-12h-18Z"), "01:48's swell is T+12: %s",
          product_of(slot_at(1, 48)));

    /* The night of 17 to 18 Nov at F4OLI's station: Northwood's 00Z analysis at 04:00 and its 24 h forecast at 05:12 on
     * 2618.5 kHz, outside any slot until then (saved with no name); 09:00 on 8040 kHz is the 06Z analysis, the one
     * 10:00 sends again: the best copy of the two is kept */
    CHECK(!strcmp(product_of(slot_at(4, 0)), "NW-surface-analysis-00Z") &&
              wefax_plan_slot(slot_at(4, 0))->freq_hz == 2618500 &&
              !strcmp(product_of(slot_at(5, 12)), "NW-surface-forecast-24h-00Z") &&
              wefax_plan_slot(slot_at(5, 12))->freq_hz == 2618500,
          "04:00 %s, 05:12 %s", product_of(slot_at(4, 0)), product_of(slot_at(5, 12)));
    CHECK(!strcmp(product_of(slot_at(9, 0)), "NW-surface-analysis-06Z") &&
              wefax_plan_slot(slot_at(9, 0))->freq_hz == 8040000 &&
              !strcmp(product_of(slot_at(9, 0)), product_of(slot_at(10, 0))),
          "09:00 %s, as 10:00", product_of(slot_at(9, 0)));

    int unknown = 0, fillers = 0;

    for (int i = 0; i < wefax_plan_count(); i++) {
        unknown += wefax_plan_product_unique(wefax_plan_slot(i)->product);
        fillers += wefax_plan_slot(i)->fill;
    }
    CHECK(!unknown && !fillers, "unknown slots %d, fillers %d", unknown, fillers);
    CHECK(wefax_bulletin_has(days, "NW-surface-forecast-120h-12Z") && !wefax_bulletin_has(days, "NW-surface-forecast-24h-12Z"),
          "Forecast 3-5 days takes 120 h, not 24 h");

    /* every slot of a kind is a chart the plan has: none of the match strings is a typo */
    for (int k = 0; k < wefax_plan_bulletin_count(); k++) {
        const wefax_bulletin_t *b = wefax_plan_bulletin(k);

        for (int m = 0; m < b->n; m++) {
            bool found = false;

            for (int i = 0; i < wefax_plan_count() && !found; i++)
                found = !strncmp(wefax_plan_slot(i)->product, b->match[m], strlen(b->match[m]));
            CHECK(found, "%s: %s matches a chart of the plan", b->name, b->match[m]);
        }
    }

    int wait, i = wefax_plan_next_of_kind(ice, hms(5, 0, 0), &wait);

    CHECK(i == slot_at(15, 20) && wait == hms(10, 20, 0), "the next ice chart after 05:00: %s in %d s",
          product_of(i), wait);
    i = wefax_plan_next_of_kind(ice, hms(22, 0, 0), &wait);
    CHECK(i == slot_at(15, 20) && wait == hms(17, 20, 0), "after 22:00 tomorrow's: %s", product_of(i));
}

/* What comes next of a kind, of a chart, of any; a chart in words; the quality in words; the copy the Bulletin shows */
static void test_words(void)
{
    wefax_plan_load();

    const wefax_bulletin_t *ice = NULL;

    for (int k = 0; k < wefax_plan_bulletin_count(); k++)
        if (!strcmp(wefax_plan_bulletin(k)->name, "Ice"))
            ice = wefax_plan_bulletin(k);

    int slot[4], wait[4], n = wefax_plan_coming(ice, NULL, hms(5, 0, 0), slot, wait, 4);

    CHECK(n == 4 && slot[0] == slot_at(15, 20) && slot[1] == slot_at(15, 40) && slot[2] == slot_at(21, 0) &&
              slot[3] == slot_at(21, 15) && wait[0] == hms(10, 20, 0) && wait[3] == hms(16, 15, 0),
          "the ice charts after 05:00, soonest first: %d of them", n);
    n = wefax_plan_coming(NULL, "NW-surface-analysis-12Z", hms(12, 0, 0), slot, wait, 4);
    CHECK(n == 4 && slot[0] == slot_at(14, 36) && slot[1] == slot_at(15, 0) && slot[2] == slot_at(16, 0) &&
              slot[3] == slot_at(17, 0),
          "Northwood's 12Z analysis after 12:00: %s first", n ? product_of(slot[0]) : "none");
    n = wefax_plan_coming(NULL, NULL, hms(23, 50, 0), slot, wait, 3);
    CHECK(n == 3 && slot[0] == slot_at(0, 0) && wait[0] == 600 && slot[1] == slot_at(0, 12) && slot[2] == slot_at(0, 36),
          "any chart after 23:50: tomorrow's 00:00 in %d s", n ? wait[0] : -1);
    CHECK(wefax_plan_coming(NULL, "XX-none", 0, slot, wait, 4) == 0, "a chart not in the plan: none coming");
    n = wefax_plan_coming(NULL, NULL, hms(4, 30, 0), slot, wait, 1);
    CHECK(n == 1 && slot[0] == slot_at(4, 30) && wait[0] == 0, "starting now: wait 0");

    /* every chart of the firmware's plan has words */
    char        hours[16];
    const char *kind;

    for (int i = 0; i < wefax_plan_count(); i++) {
        kind = wefax_plan_chart_kind(product_of(i), hours, sizeof(hours));
        CHECK(kind != NULL, "%s has words", product_of(i));
    }

    static const struct {
        const char *product, *kind, *hours;
    } words[] = {
        {"NW-surface-analysis-12Z", "Analysis", ""},
        {"DWD-surface-analysis-Atlantic-00Z", "Analysis, North Atlantic", ""},
        {"NW-surface-forecast-24h-06Z", "Forecast", "24 h"},
        {"DWD-pressure-forecast-108h-00Z", "Forecast", "108 h"},
        {"DWD-850hPa-forecast-36-48h-00Z", "Upper air", "36-48 h"},     /* not "850 h" */
        {"NW-sea-swell-12h-18Z", "Sea and swell", "12 h"},
        {"DWD-waves-48h-12Z", "Sea and swell", "48 h"},
        {"NW-surface-wind-24h-06Z", "Wind", "24 h"},
        {"NW-rain-visibility-12h-00Z", "Rain and visibility", "12 h"},
        {"NW-upper-sigwx-18Z", "Significant weather", ""},
        {"DWD-sigwx-analysis-12Z", "Significant weather", ""},          /* saved before its slot was renamed */
        {"DWD-ice-NW-Atlantic-12Z", "Ice", ""},
        {"NW-unknown", "Unsorted", ""},
        {"", "Unsorted", ""},
    };

    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        kind = wefax_plan_chart_kind(words[i].product, hours, sizeof(hours));
        CHECK(kind && !strcmp(kind, words[i].kind) && !strcmp(hours, words[i].hours), "%s: %s %s",
              words[i].product, kind ? kind : "(none)", hours);
    }
    CHECK(!wefax_plan_chart_kind("XX-something-else", hours, sizeof(hours)) && !hours[0], "a name it does not know");

    CHECK(!strcmp(wefax_plan_quality_word(91), "good") && !strcmp(wefax_plan_quality_word(70), "good") &&
              !strcmp(wefax_plan_quality_word(69), "fair") && !strcmp(wefax_plan_quality_word(30), "fair") &&
              !strcmp(wefax_plan_quality_word(29), "poor") && !strcmp(wefax_plan_quality_word(0), "poor") &&
              !wefax_plan_quality_word(-1)[0],
          "the quality in words: good from 70, fair from 30, poor under");

    /* the Bulletin's copy: the newest of fair mark, a poor one only when there is no other */
    const char *old_good = "wefax-20261116-062315-NW-surface-analysis-00Z-q91.png";
    const char *new_poor = "wefax-20261116-120112-NW-surface-analysis-06Z-q12.png";
    const char *new_fair = "wefax-20261116-123012-DWD-surface-analysis-06Z-q35.png";
    const char *older_poor = "wefax-20261116-050000-NW-surface-analysis-00Z-q8.png";

    CHECK(wefax_plan_bulletin_prefers(new_poor, ""), "anything rather than none");
    CHECK(!wefax_plan_bulletin_prefers(new_poor, old_good) && wefax_plan_bulletin_prefers(old_good, new_poor),
          "an older good copy rather than a newer poor one");
    CHECK(wefax_plan_bulletin_prefers(new_fair, old_good) && !wefax_plan_bulletin_prefers(old_good, new_fair),
          "the newer of two fair or better");
    CHECK(wefax_plan_bulletin_prefers(new_poor, older_poor) && !wefax_plan_bulletin_prefers(older_poor, new_poor),
          "the newer of two poor ones");
    CHECK(wefax_plan_bulletin_prefers(new_poor, "wefax-20261101-120000.png"),
          "a poor copy and one from before the marks: the newer");
}

/* ---- previews ---- */

static bool write_png(const char *path, const uint8_t *img, unsigned w, unsigned h)
{
    FILE       *fp  = fopen(path, "wb");
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;

    if (!fp || !png || !info || setjmp(png_jmpbuf(png))) {
        if (fp)
            fclose(fp);
        png_destroy_write_struct(&png, &info);
        return false;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (unsigned y = 0; y < h; y++)
        png_write_row(png, (png_bytep)(img + (size_t)y * w));
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

static void test_previews(void)
{
    char     dir[512], path[700];
    unsigned w = 1809, h = 1300, pw = 0, ph = 0;
    uint8_t *img = malloc((size_t)w * h);

    snprintf(dir, sizeof(dir), "%s/wefax", BOARD_DATA);
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
            img[(size_t)y * w + x] = x < w / 2 ? 0 : 255;       /* black left, white right */

    const char *name = "wefax-20261116-120000-NW-surface-analysis-12Z-q80.png";

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    CHECK(write_png(path, img, w, h), "a chart written");

    unsigned rw, rh;
    uint8_t *back = wefax_png_read_grey(path, &rw, &rh);

    CHECK(back && rw == w && rh == h && back[10] == 0 && back[w - 10] == 255, "read back grey, %u x %u", rw, rh);
    free(back);

    CHECK(!wefax_preview_load(dir, name, &pw, &ph), "no preview before it is made");
    CHECK(wefax_preview_make(dir, name), "the preview made");

    uint8_t *prv = wefax_preview_load(dir, name, &pw, &ph);

    CHECK(prv && pw <= WEFAX_PREVIEW_W && ph <= WEFAX_PREVIEW_H && (pw == WEFAX_PREVIEW_W || ph == WEFAX_PREVIEW_H),
          "the preview %u x %u fits", pw, ph);
    CHECK(prv && abs((int)pw * (int)h - (int)ph * (int)w) <= (int)w, "it keeps the chart's shape");
    CHECK(prv && prv[(size_t)(ph / 2) * pw + 2] == 0 && prv[(size_t)(ph / 2) * pw + pw - 3] == 255,
          "black left, white right");
    free(prv);

    /* the chart deleted elsewhere: its preview swept, another's kept */
    const char *other = "wefax-20261116-130000-NW-surface-analysis-12Z-q85.png";

    snprintf(path, sizeof(path), "%s/%s", dir, other);
    write_png(path, img, w, h);
    wefax_preview_make(dir, other);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unlink(path);
    CHECK(wefax_preview_sweep(dir) == 1, "one preview swept");
    CHECK(!wefax_preview_load(dir, name, &pw, &ph), "the deleted chart's preview is gone");
    prv = wefax_preview_load(dir, other, &pw, &ph);
    CHECK(prv != NULL, "the other's is there");
    free(prv);
    wefax_preview_remove(dir, other);
    CHECK(!wefax_preview_load(dir, other, &pw, &ph), "removed with its chart");

    CHECK(!wefax_png_read_grey("/nonexistent.png", &rw, &rh), "no file: NULL");
    free(img);
}

/* ---- the quality mark, on charts made up here ---- */

static double noise(void)
{
    return (rand() / (double)RAND_MAX + rand() / (double)RAND_MAX + rand() / (double)RAND_MAX - 1.5) * 2.0;
}

/* A chart: white, a frame, a grid every 120 px, isobar-like curves, a block of text-like dashes; snr 0: clean, else
 * that much grey noise (its sigma) and that share of rows faded */
static uint8_t *chart(unsigned rows, double sigma, double faded)
{
    uint8_t *img = malloc((size_t)WIDTH * rows);

    for (unsigned y = 0; y < rows; y++) {
        bool fade = faded > 0 && rand() / (double)RAND_MAX < faded;

        for (unsigned x = 0; x < WIDTH; x++) {
            double v = 235;

            if (x < 6 || x >= WIDTH - 6 || y < 6 || y >= rows - 6 || x % 120 < 2 || y % 120 < 2)
                v = 20;
            for (int c = 0; c < 6; c++) {
                double cy = 200 + c * 180 + 60 * sin((x + c * 300) / 160.0);

                if (fabs(y - cy) < 2.0)
                    v = 30;
            }
            if (y > 40 && y < 70 && x > 100 && x < 700 && (x / 9) % 3 && (y / 6) % 2)
                v = 25;
            v += sigma * noise();
            if (fade)
                v = v * 0.4 + 60;
            img[(size_t)y * WIDTH + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    return img;
}

static void test_quality(void)
{
    srand(1);

    uint8_t *clean = chart(1400, 0, 0), *noisy = chart(1400, 60, 0.15), *cut = chart(500, 0, 0);
    uint8_t *junk  = malloc((size_t)WIDTH * 1400);

    for (size_t i = 0; i < (size_t)WIDTH * 1400; i++)
        junk[i] = (uint8_t)(rand() % 256);

    float q_clean = wefax_quality(clean, WIDTH, 1400, false, NULL);
    float q_stop  = wefax_quality(clean, WIDTH, 1400, true, NULL);
    float q_noisy = wefax_quality(noisy, WIDTH, 1400, false, NULL);
    float q_cut   = wefax_quality(cut, WIDTH, 500, false, NULL);
    float q_junk  = wefax_quality(junk, WIDTH, 1400, false, NULL);

    printf("  marks: clean %.1f, without its stop %.1f, noisy and faded %.1f, cut short %.1f, noise %.1f\n", q_clean,
           q_stop, q_noisy, q_cut, q_junk);
    CHECK(q_clean >= 60, "a clean chart marks well: %.1f", q_clean);
    CHECK(fabsf(q_stop - 0.8f * q_clean) < 0.1f, "without its stop tone x 0.8: %.1f", q_stop);
    CHECK(q_noisy < q_clean - 10, "noise and fading mark it down: %.1f", q_noisy);
    CHECK(q_cut < q_clean / 2, "a chart cut short marks low: %.1f", q_cut);
    CHECK(q_junk < 15, "noise is no chart: %.1f", q_junk);
    CHECK(q_clean > q_noisy && q_noisy > q_junk, "the best copy is the clean one");

    /* the quick look of a chart joined: 30 rows */
    float dark, c = wefax_quality_rows(clean + (size_t)300 * WIDTH, WIDTH, 30, &dark);

    CHECK(c >= 0.35f && dark <= 0.45f, "30 rows of a chart: coherence %.2f, dark %.2f", c, dark);
    c = wefax_quality_rows(junk, WIDTH, 30, &dark);
    CHECK(c < 0.35f || dark > 0.45f, "30 rows of noise: coherence %.2f, dark %.2f", c, dark);

    free(clean);
    free(noisy);
    free(cut);
    free(junk);
}

/* ---- Auto Tilt ---- */

/* The chart as it comes in when the station's line rate and ours differ by tilt: the rows that the app's Tilt (the PNG
 * writer's shift, wefax_window.c tilt_px) straightens again */
static uint8_t *sheared(const uint8_t *straight, unsigned w, unsigned h, int tilt)
{
    uint8_t *raw = malloc((size_t)w * h);

    for (unsigned y = 0; y < h; y++) {
        int shift = (int)(((int64_t)y - (int64_t)(h - 1)) * tilt * WEFAX_TILT_UNIT_W / (1000LL * w));

        for (unsigned x = 0; x < w; x++) {
            int from = ((int)x + shift) % (int)w;

            raw[(size_t)y * w + x] = straight[(size_t)y * w + (from < 0 ? from + (int)w : from)];
        }
    }
    return raw;
}

static void test_tilt(void)
{
    srand(2);

    unsigned w = WIDTH, h = 1300;
    uint8_t *straight = chart(h, 25, 0);
    int      tilts[]  = {-185, -10, 0, 120, 333};

    for (size_t i = 0; i < sizeof(tilts) / sizeof(tilts[0]); i++) {
        uint8_t *raw = sheared(straight, w, h, tilts[i]);
        int      t   = 9999;
        bool     ok  = wefax_auto_tilt(raw, w, h, &t);

        CHECK(ok && abs(t - tilts[i]) <= 2, "a chart received with tilt %+d: found %+d (%s)", tilts[i], t,
              ok ? "sure" : "not sure");
        free(raw);
    }

    /* nothing to go by: a blank page */
    uint8_t *blank = malloc((size_t)w * h);
    int      t;

    memset(blank, 235, (size_t)w * h);
    CHECK(!wefax_auto_tilt(blank, w, h, &t), "a blank page has no tilt");
    CHECK(!wefax_auto_tilt(blank, w, 10, &t), "nor 10 rows");
    free(blank);
    free(straight);

    /* a station's tilt: the median of its last five */
    int learn[] = {-180, -185, -190, -186, -184, -300};

    CHECK(!wefax_station_tilt("DWD", &t), "none known yet");
    for (size_t i = 0; i < sizeof(learn) / sizeof(learn[0]); i++)
        wefax_station_tilt_learn("DWD", learn[i]);
    wefax_station_tilt_learn("Northwood", -10);
    wefax_station_tilt_learn("Bad name", 50);
    CHECK(wefax_station_tilt("DWD", &t) && t == -186, "DWD: the median of the last five, %+d", t);
    CHECK(wefax_station_tilt("Northwood", &t) && t == -10, "Northwood %+d", t);
    CHECK(!wefax_station_tilt("Bad name", &t) && !wefax_station_tilt("", &t), "a name with a space is not kept");
}

/* A chart written with Align and Tilt (Fix, Auto Plan's save) comes out as the app shows it */
static void test_write_fixed(void)
{
    srand(3);

    unsigned w = WIDTH, h = 600, rw = 0, rh = 0;
    uint8_t *straight = chart(h, 10, 0), *raw = sheared(straight, w, h, -185), *back;
    char     path[600], part[620];
    struct stat st;

    snprintf(path, sizeof(path), "%s/wefax/wefax-20261117-101500-NW-surface-analysis-06Z-q80.png", BOARD_DATA);
    snprintf(part, sizeof(part), "%s.part", path);
    CHECK(wefax_png_write_fixed(path, raw, w, h, 0, -185), "written with Tilt -185");
    CHECK(stat(part, &st) != 0, "no .part left");
    back = wefax_png_read_grey(path, &rw, &rh);
    CHECK(back && rw == w && rh == h && !memcmp(back, straight, (size_t)w * h), "Tilt straightens the received chart");
    free(back);

    /* Align: each row moved right by it, round the width */
    CHECK(wefax_png_write_fixed(path, straight, w, h, 100, 0), "written with Align +100");
    back = wefax_png_read_grey(path, &rw, &rh);
    CHECK(back && back[(size_t)10 * w + 150] == straight[(size_t)10 * w + 50] &&
              back[(size_t)10 * w + 20] == straight[(size_t)10 * w + w - 80],
          "Align +100 moves the chart 100 px right");
    free(back);
    CHECK(!wefax_png_write_fixed("/nonexistent/dir/x.png", straight, w, h, 0, 0), "an unwritable folder: false");
    unlink(path);
    free(raw);
    free(straight);
}

/* Night: On and Off whatever the sun, Auto by the sun at the locator (6 degrees under), never without a locator or a
 * set clock; the dimming */
static void test_night(void)
{
    struct tm tm = {.tm_year = 2026 - 1900, .tm_mon = 10, .tm_mday = 17, .tm_hour = 12};
    time_t    noon = timegm(&tm), t;

    /* JO20 (Belgium) in November: day at 12 UTC, night at 21 UTC, still light at 16:15 UTC (sunset about 15:50,
     * 6 degrees under some 40 min later) */
    CHECK(!wefax_night_at(WEFAX_NIGHT_AUTO, "JO20", noon), "Auto at noon in JO20: day");
    CHECK(wefax_night_at(WEFAX_NIGHT_AUTO, "JO20", noon + 9 * 3600), "Auto at 21 UTC in JO20: night");
    CHECK(!wefax_night_at(WEFAX_NIGHT_AUTO, "JO20ab", noon + 4 * 3600 + 15 * 60), "16:15 UTC: dusk, not yet night");
    CHECK(wefax_night_at(WEFAX_NIGHT_AUTO, "JO20ab", noon + 5 * 3600), "17:00 UTC: night");
    /* the other side of the world: night at noon UTC in New Zealand's winter? November is its summer, noon UTC is
     * 01:00 there: night */
    CHECK(wefax_night_at(WEFAX_NIGHT_AUTO, "RF73", noon), "Auto at 01 local in RF73: night");
    CHECK(!wefax_night_at(WEFAX_NIGHT_AUTO, "RF73", noon - 10 * 3600), "Auto at 15 local in RF73: day");

    CHECK(wefax_night_at(WEFAX_NIGHT_ON, "JO20", noon) && wefax_night_at(WEFAX_NIGHT_ON, NULL, 0), "On: always");
    CHECK(!wefax_night_at(WEFAX_NIGHT_OFF, "JO20", noon + 9 * 3600), "Off: never");
    CHECK(!wefax_night_at(WEFAX_NIGHT_AUTO, NULL, noon + 9 * 3600) &&
              !wefax_night_at(WEFAX_NIGHT_AUTO, "", noon + 9 * 3600) &&
              !wefax_night_at(WEFAX_NIGHT_AUTO, "ZZ99", noon + 9 * 3600),
          "Auto without a valid locator: never");
    t = 86400 * 3 + 21 * 3600;      /* 4 Jan 1970, 21 UTC: the clock not set */
    CHECK(!wefax_night_at(WEFAX_NIGHT_AUTO, "JO20", t), "Auto with the clock not set: never");
    CHECK(!wefax_night_at(7, "JO20", noon + 9 * 3600), "an unknown setting: never");

    CHECK(wefax_night_grey(255, false) == 255 && wefax_night_grey(255, true) == 106 &&
              wefax_night_grey(0, true) == 0 && wefax_night_grey(128, true) == 53,
          "white dims to %d, black stays", wefax_night_grey(255, true));
}

int main(void)
{
    test_load();
    test_next();
    test_fax();
    test_names();
    test_slot_of_file();
    test_bulletin();
    test_words();
    test_previews();
    test_quality();
    test_tilt();
    test_write_fixed();
    test_night();
    printf("wefax plan: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
