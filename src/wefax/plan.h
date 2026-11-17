/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax Auto Plan: the day's slots (UTC), one a chart the stations send, each on the frequency that
 *  comes in best at its hour, and the kinds of chart the Bulletin shows. The firmware's plan
 *  (tools/wefax_plan/plan.json, built in by plan_to_c.py) unless BOARD_DATA/wefax/plan.json replaces it.
 *
 *  Here the plan and its decisions, from the times and names given: which slot comes next, which slot a fax is,
 *  whether a fax coming in keeps the radio when the next slot comes, a chart going on worth joining, the names a fax
 *  is saved under and the best copy of a chart. wefax_window.c reads the clock, tunes and saves; the host test
 *  (tools/wefax_plan) drives these.
 *
 *  From OlivLoze's fork (F4OLI): the plan of his 48 h watch and the way Auto Plan follows it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFAX_PLAN_MAX          256

#define WEFAX_PLAN_EARLY_S      10      /* a slot is tuned this long before its start */
#define WEFAX_PLAN_LATE_S       90      /* a slot a slow tick missed is still taken this late */
#define WEFAX_PLAN_AGAIN_S      3600    /* a slot taken (or skipped) is not taken again within this */
#define WEFAX_PLAN_SAME_S       (16 * 3600)     /* copies of one chart this close are the same chart sent again */
#define WEFAX_PLAN_JOIN_LEFT_S  120     /* a chart going on is joined only with this much of it still to come */
#define WEFAX_PLAN_END_S        30      /* a fax of the plan keeps the radio until this close to its slot's end */
#define WEFAX_PLAN_DUR_S        690     /* a chart's length when the plan gives none */

typedef struct {
    uint16_t min;           /* start, minutes after 00:00 UTC */
    uint16_t dur;           /* the longest the chart lasts, s */
    int32_t  freq_hz;       /* the station's frequency (the dial is WEFAX_CENTER_HZ below, USB) */
    bool     fill;          /* a filler: gives way to a slot of the plan */
    char     station[16];
    char     product[48];   /* the chart: its copies within WEFAX_PLAN_SAME_S are one chart */
} wefax_slot_t;

/* Reads the plan (the file, else the firmware's); the number of slots, sorted by start */
int wefax_plan_load(void);
/* The plan from a text instead (the host test): the number of slots, -1 when it is no plan */
int wefax_plan_parse(const char *json);

int                 wefax_plan_count(void);
const wefax_slot_t *wefax_plan_slot(int i);
bool                wefax_plan_from_file(void);

/* A chart whose copies are all kept: one with no name, or "unknown" in it */
bool wefax_plan_product_unique(const char *product);

/* ---- the times: sod is the second of the UTC day, 0..86399 ---- */

int wefax_plan_sod(time_t t);

/* Seconds since slot s started at sod, -43200..43199 (negative: it is still to come) */
int wefax_plan_since(const wefax_slot_t *s, int sod);

/* The slot to take next at sod: of those not taken in the last WEFAX_PLAN_AGAIN_S (done[i]: when slot i was taken or
 * skipped, 0 never; now the time), the one whose tuning (WEFAX_PLAN_EARLY_S before its start) comes first, or passed
 * by WEFAX_PLAN_LATE_S at most. *wait gets the seconds until its tuning, 0 or less: now. -1 for none */
int wefax_plan_next(int sod, time_t now, const time_t *done, int *wait);

/* The slot a fax confirmed at sod on freq_hz (the station's frequency) belongs to: the one at that frequency whose
 * start is nearest, from 60 s before it to 180 s after; -1 for none */
int wefax_plan_slot_of_fax(int32_t freq_hz, int sod);

/* The slot whose chart started last before sod, its seconds since in *since; -1 for an empty plan */
int wefax_plan_last_started(int sod, int *since);

/* Opened since seconds into slot s's chart: enough of it is still to come to take it from there */
bool wefax_plan_worth_joining(const wefax_slot_t *s, int since);

/* Slot next's time comes with a fax coming in for elapsed s, of slot cur (NULL: one that is no slot of the plan).
 * True: the fax keeps the radio and next is skipped. A fax of the plan is never cut for another chart, only a filler
 * or a fax that is none of the plan's gives way, and only to a chart of the plan; a fax past its slot's end gives way
 * to any */
bool wefax_plan_keeps_radio(const wefax_slot_t *cur, int elapsed, const wefax_slot_t *next);

/* ---- the names: wefax-YYYYMMDD-HHMMSS[-<chart>]-q<mark>.png, the date and time local, as the clock shows ---- */

/* The name of a fax saved at local time tm, of the chart product ("" for none), its mark q */
void wefax_plan_file_name(char *out, size_t size, const struct tm *tm, const char *product, int q);

/* When a fax was saved, from its name; -1 when it is not one of ours */
time_t wefax_plan_name_time(const char *name);
/* Its mark, -1 for none (a fax saved before marks) */
int wefax_plan_name_mark(const char *name);
/* Its chart: the name's middle, "" for none */
void wefax_plan_name_product(const char *name, char *out, size_t size);

/* The slot a saved chart came from: its chart, the last such slot started before it was saved (UTC); NULL none */
const wefax_slot_t *wefax_plan_slot_of_file(const char *name);

/* The best mark of the copies of product saved within WEFAX_PLAN_SAME_S of when in dir, -1 for none. With drop set,
 * the copies other than keep are deleted instead, each named to dropped (a better one was just written) */
int wefax_plan_copies(const char *dir, const char *product, time_t when, const char *keep, bool drop,
                      void (*dropped)(const char *dir, const char *name));

/* ---- the Bulletin: the kinds of chart that matter on a trip (analysis, forecasts, sea...), each the charts that
 * start with one of its match strings, in the plan's order ---- */

#define WEFAX_BULLETIN_MAX      16
#define WEFAX_BULLETIN_MATCH    6

typedef struct {
    char name[24];
    char match[WEFAX_BULLETIN_MATCH][40];
    int  n;
} wefax_bulletin_t;

int                     wefax_plan_bulletin_count(void);
const wefax_bulletin_t *wefax_plan_bulletin(int i);
bool                    wefax_bulletin_has(const wefax_bulletin_t *b, const char *product);

/* The slot that brings kind b next after sod, the seconds until its start in *wait (0..86399); -1 for none */
int wefax_plan_next_of_kind(const wefax_bulletin_t *b, int sod, int *wait);

/* The slots from sod on (the next day's too) that bring kind b, or the chart product when b is NULL, or any chart
 * when both are NULL: soonest first, at most max, each slot's index in slot[] and the seconds until its start
 * (0..86399) in wait[]. How many */
int wefax_plan_coming(const wefax_bulletin_t *b, const char *product, int sod, int *slot, int *wait, int max);

/* ---- the copies: their quality and which one shows ---- */

#define WEFAX_MARK_GOOD 70      /* a copy's mark (wefax/quality.h): "good" from this */
#define WEFAX_MARK_FAIR 30      /* "fair" from this, "poor" under it: noise, a fade */

/* The mark in a word: "good", "fair" or "poor", "" for none (q < 0); English, the GUI translates it */
const char *wefax_plan_quality_word(int q);

/* Of two saved copies of a kind of chart (file names; have "" for none), the one the Bulletin shows: the newest of
 * fair mark, a poor one only when there is no other, else the newest. True: cand rather than have */
bool wefax_plan_bulletin_prefers(const char *cand, const char *have);

/* A chart in words, from its name: the kind ("Analysis", "Forecast", "Upper air", "Sea and swell", "Unsorted" for
 * none or an unknown chart...; the Bulletin's words where it has the same), English for the GUI to translate, and its
 * hours ("48 h", "36-48 h", "" none). NULL for a name it does not know: show the name itself */
const char *wefax_plan_chart_kind(const char *product, char *hours, size_t size);

#ifdef __cplusplus
}
#endif
