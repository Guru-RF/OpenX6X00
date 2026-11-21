/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of APP > Weather > Notices (src/meteo_alerts.c, compiled in from a copy in build/ with the stand-ins of
 *  stub_include/; see the Makefile). The clock is the test's own (--wrap=time); the helper is a stand-in script in
 *  build/sbin that writes the test's answer, so nothing goes to any service. The cases:
 *  - off (the default): never a check, never a notice;
 *  - on: the first check 2 minutes after the start, then every 30 minutes, never while APP > Weather is open; the
 *    helper's arguments (the position, the country, the source, the language, the answer file);
 *  - which warnings make a notice: the place's own, from a service yellow and up, worked out orange and up, not over;
 *  - a notice only while receiving (not in TX, the tuner, the SWR scan), with the screen lit and APP > Weather shut:
 *    held back till then, one at a time, each once, also after a restart (/mnt/meteo/seen), and none for a warning
 *    the app has shown; those over leave the list; a helper's error: nothing; off: those held back dropped;
 *  - in US units (Weather units, #315) a worked-out one in them: "gusts up to 59 mph".
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "meteo_alerts.c"

#include "countries.h"

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- the stand-ins ---- */

static volatile time_t fake_now;

time_t __wrap_time(time_t *t) {
    if (t) *t = fake_now;
    return fake_now;
}

static ParamInt notices = {0};
ParamInt       *cfg_meteo_alerts = &notices;

int32_t param_i_get(const ParamInt *p) {
    return p->v;
}

static ParamInt from = {0};     /* Weather from: where the radio is */
ParamInt       *cfg_meteo_from  = &from;
ParamText      *cfg_meteo_place = NULL;
static ParamInt units = {0};    /* Weather units: metric */
ParamInt       *cfg_meteo_units = &units;

size_t param_t_get(const ParamText *p, char *out, size_t size) {
    (void)p;
    if (size) out[0] = 0;
    return 0;
}

static dialog_t meteo_app;
dialog_t       *dialog_meteo = &meteo_app;

static radio_state_t state = RADIO_RX;
static bool          lit   = true;

radio_state_t radio_get_state() {
    return state;
}

bool backlight_seen() {
    return lit;
}

static lv_timer_cb_t timer;

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data) {
    (void)period;
    (void)user_data;
    timer = cb;
    return NULL;
}

static char shown[8][200];
static int  shown_n;

void msg_schedule_text_ms(uint16_t ms, const char *fmt, ...) {
    va_list ap;

    (void)ms;
    va_start(ap, fmt);
    if (shown_n < 8) vsnprintf(shown[shown_n++], sizeof(shown[0]), fmt, ap);
    va_end(ap);
}

void msg_schedule_text_fmt(const char *fmt, ...) {
    (void)fmt;
}

/* The LVGL thread's queue: what the helper's thread hands back, run when the test pumps it */
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static scheduler_fn_t  q_fn;
static char            q_arg[16];

bool scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size) {
    pthread_mutex_lock(&q_lock);
    memcpy(q_arg, arg, arg_size < sizeof(q_arg) ? arg_size : sizeof(q_arg));
    q_fn = fn;
    pthread_mutex_unlock(&q_lock);
    return true;
}

/* The check's answer back: true when it came within 5 s */
static bool pump(void) {
    for (int i = 0; i < 500; i++) {
        scheduler_fn_t fn;

        pthread_mutex_lock(&q_lock);
        fn   = q_fn;
        q_fn = NULL;
        pthread_mutex_unlock(&q_lock);
        if (fn) {
            fn(q_arg);
            return true;
        }
        usleep(10000);
    }
    return false;
}

/* meteo.c's: a GPS fix in Brussels, Belgium set, the GUI in English */
bool station_position(double *lat, double *lon, const char **src) {
    *lat = 50.8467;
    *lon = 4.3525;
    *src = "GPS";
    return true;
}

bool station_locator_position(double *lat, double *lon) {
    (void)lat;
    (void)lon;
    return false;
}

static const country_t belgium = {"BE", 1, "CET-1CEST,M3.5.0,M10.5.0/3", 50.8333f, 4.3333f};

const country_t *country_now(void) {
    return &belgium;
}

const char *country_code(char *cc) {
    snprintf(cc, 3, "%s", belgium.cc);
    return cc;
}

const char *country_name(const country_t *c) {
    return c ? "Belgium" : "";
}

const char *i18n_code(void) {
    return "";
}

const char *i18n_tr(const char *s) {
    return s;
}

/* ---- the helper's answer, its runs ---- */

#define DIR      BOARD_DATA
#define ANSWER   DIR "/answer.json"
#define RUNS     DIR "/runs"
#define T0       1794834000 /* Mon 16 Nov 2026 12:00 UTC */

/* One warning of the answer */
static void warn(FILE *f, bool first, const char *id, bool mine, const char *kind, int level, long end,
                 const char *calc) {
    fprintf(f, "%s{\"id\":\"%s\",\"src\":\"%s\",\"kind\":\"%s\",\"level\":%d,\"mine\":%s,\"end\":%ld,"
               "\"event\":\"Wind %s\",\"from\":\"2026-11-16 13:00\",\"to\":\"2026-11-17 09:00\"%s%s%s}",
            first ? "" : ",", id, kind[0] == 'c' ? "forecast" : "MeteoAlarm", kind, level, mine ? "true" : "false",
            end, id, calc ? ",\"calc\":\"" : "", calc ? calc : "", calc ? "\",\"value\":95,\"day\":0" : "");
}

/* The warnings in force from T0 + 7200, when the notices are turned on; more_id: one more, orange, from the service
 * or (more_calc) worked out */
static void answer(const char *more_id, const char *more_calc) {
    FILE *f = fopen(ANSWER, "w");
    long  on = T0 + 7200;

    fprintf(f, "{\"tz_abbr\":\"CET\",\"alerts\":[");
    warn(f, true, "a1", true, "official", 2, on + 3600, NULL);       /* yellow, here: a notice */
    warn(f, false, "b1", true, "official", 1, on + 3600, NULL);      /* minor: none */
    warn(f, false, "c1", false, "official", 4, on + 3600, NULL);     /* red, but nearby: none */
    warn(f, false, "d1", true, "computed", 2, on + 3600, "gust");    /* worked out, yellow: none */
    warn(f, false, "e1", true, "computed", 3, on + 7200, "gust");    /* worked out, orange: a notice */
    warn(f, false, "f1", true, "official", 4, on - 10, NULL);        /* over: none */
    warn(f, false, "", true, "official", 3, on + 3600, NULL);        /* no id: none */
    warn(f, false, "h1", true, "official", 3, 0, NULL);              /* no end: a notice */
    if (more_id) warn(f, false, more_id, true, more_calc ? "computed" : "official", 3, T0 + 30000, more_calc);
    fprintf(f, "]}\n");
    fclose(f);
}

static int runs(char *last, size_t size) {
    FILE *f = fopen(RUNS, "r");
    char  line[400];
    int   n = 0;

    if (last) last[0] = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        n++;
        if (last) snprintf(last, size, "%.*s", (int)strcspn(line, "\n"), line);
    }
    fclose(f);
    return n;
}

static void tick(time_t at) {
    fake_now = at;
    timer(NULL);
}

static bool seen_file_has(const char *id) {
    FILE *f = fopen(SEEN_FILE, "r");
    char  line[64];
    bool  yes = false;

    if (!f) return false;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, id, strlen(id)) && line[strlen(id)] == ' ') yes = true;
    fclose(f);
    return yes;
}

/* After a restart: the module's state as at the start, the seen list read back */
static void restart(void) {
    seen_n = waiting_n = 0;
    running = false;
    next_at = 0;
    meteo_alerts_init();
}

int main(void) {
    char last[400], want[400];

    unlink(RUNS);
    unlink(SEEN_FILE);
    unlink(METEO_ALERTS_FILE);
    setenv("METEO_TEST_ANSWER", ANSWER, 1);
    setenv("METEO_TEST_RUNS", RUNS, 1);
    fake_now = T0;
    meteo_alerts_init();
    check(timer != NULL, "a timer of its own");

    /* Off, the default */
    for (time_t t = T0; t < T0 + 7200; t += 60) tick(t);
    check(runs(NULL, 0) == 0 && shown_n == 0, "off: no check in 2 hours, no notice");

    /* On: the first check 2 minutes after */
    fake_now = T0 + 7200;
    answer(NULL, NULL);
    notices.v = 1;
    tick(T0 + 7200);
    tick(T0 + 7200 + 119);
    check(runs(NULL, 0) == 0, "on: no check before 2 minutes");
    tick(T0 + 7200 + 120);
    check(pump(), "the check at 2 minutes, its answer back");
    snprintf(want, sizeof(want), "alerts 50.84670 4.35250 BE GPS en %s", METEO_ALERTS_FILE);
    check(runs(last, sizeof(last)) == 1 && !strcmp(last, want), "the helper's arguments: \"%s\"", last);
    check(waiting_n == 3 && !strcmp(waiting[0].id, "a1") && !strcmp(waiting[1].id, "e1") &&
              !strcmp(waiting[2].id, "h1"),
          "a1 (yellow), e1 (worked out, orange), h1 (no end) held: %d", waiting_n);
    check(waiting[2].until == T0 + 7200 + 120 + 3 * 86400, "a warning without an end: seen for 3 days");

    /* Only while receiving, the screen lit, the app shut */
    fake_now = T0 + 7400;
    answer(NULL, NULL);
    state = RADIO_TX;
    tick(T0 + 7400);
    state = RADIO_ATU_RUN;
    tick(T0 + 7405);
    state = RADIO_SWRSCAN;
    tick(T0 + 7410);
    check(shown_n == 0, "none in TX, while tuning or scanning the SWR");
    state = RADIO_RX;
    lit   = false;
    tick(T0 + 7415);
    check(shown_n == 0, "none with the screen dark");
    lit           = true;
    meteo_app.run = true;
    tick(T0 + 7420);
    check(shown_n == 0, "none while APP > Weather is open");
    meteo_app.run = false;
    tick(T0 + 7425);
    check(shown_n == 1 && !strcmp(shown[0], "Weather warning. Yellow: Wind a1\nMon 16 13:00 to Tue 17 09:00 CET"),
          "then one: \"%s\"", shown[0]);
    tick(T0 + 7430);
    tick(T0 + 7435);
    tick(T0 + 7440);
    check(shown_n == 3 && !strcmp(shown[1], "Weather warning. Orange: gusts up to 95 km/h, from the forecast\ntoday"),
          "one at a time, three in all: \"%s\"", shown[1]);
    check(seen_file_has("a1") && seen_file_has("e1") && seen_file_has("h1") && !seen_file_has("b1"),
          "the three kept in the seen list");

    /* Every 30 minutes, the same warnings: no notice again */
    tick(T0 + 7200 + 120 + 1799);
    check(runs(NULL, 0) == 1, "no check before 30 minutes");
    tick(T0 + 7200 + 120 + 1800);
    check(pump() && runs(NULL, 0) == 2, "the check at 30 minutes");
    for (int i = 0; i < 4; i++) tick(T0 + 9200 + i * 5);
    check(shown_n == 3 && waiting_n == 0, "the same warnings: not again");

    /* Not while the app is open: it asks itself */
    meteo_app.run = true;
    tick(T0 + 7200 + 120 + 3600);
    check(runs(NULL, 0) == 2, "no check while APP > Weather is open");
    meteo_app.run = false;

    /* After a restart: read back, not again */
    restart();
    check(seen_n == 3, "the seen list read back: %d", seen_n);
    tick(T0 + 11000);
    tick(T0 + 11120);
    check(pump() && runs(NULL, 0) == 3, "the check 2 minutes after the restart");
    for (int i = 0; i < 4; i++) tick(T0 + 11125 + i * 5);
    check(shown_n == 3, "after a restart: not again");

    /* The app showed one: no notice for it */
    {
        cJSON *data = cJSON_Parse("{\"alerts\":[{\"id\":\"n1\",\"mine\":true,\"kind\":\"official\",\"level\":3,"
                                  "\"end\":1794850000}]}");

        fake_now = T0 + 12000;
        meteo_alerts_seen(data);
        cJSON_Delete(data);
    }
    check(seen_file_has("n1"), "shown by the app: in the seen list");
    answer("n1", NULL);
    tick(T0 + 11120 + 1800);
    check(pump(), "the next check");
    for (int i = 0; i < 4; i++) tick(T0 + 13000 + i * 5);
    check(shown_n == 3, "none for the warning the app showed");

    /* A new one held back, then the setting off: dropped */
    answer("x1", NULL);
    tick(T0 + 11120 + 3600);
    check(pump() && waiting_n == 1, "a new one held back");
    notices.v = 0;
    tick(T0 + 14800);
    check(waiting_n == 0 && shown_n == 3, "off: those held back dropped");
    notices.v = 1;

    /* The helper's error: nothing; the next check 30 minutes on */
    unlink(METEO_ALERTS_FILE);
    setenv("METEO_TEST_FAIL", "1", 1);
    tick(T0 + 15000);
    tick(T0 + 15120);
    check(pump() && waiting_n == 0, "the helper's error: nothing held");
    tick(T0 + 15120 + 60);
    check(runs(NULL, 0) == 6, "not asked again at once: %d runs", runs(NULL, 0));
    unsetenv("METEO_TEST_FAIL");

    /* In US units: a worked-out one in them (meteo_warning_title, as APP > Weather has it) */
    units.v = 1;
    answer("u1", "gust");
    tick(T0 + 15120 + 1800);
    check(pump() && waiting_n == 1, "a worked-out one held back");
    for (int i = 0; i < 4; i++) tick(T0 + 17000 + i * 5);
    check(shown_n == 4 && !strcmp(shown[3], "Weather warning. Orange: gusts up to 59 mph, from the forecast\ntoday"),
          "in US units: \"%s\"", shown[3]);
    units.v = 0;

    /* Those over leave the list: a1 ended at T0 + 10800, h1 (no end) is kept 3 days */
    fake_now = T0 + 20000;
    mark_seen("z1", T0 + 30000);
    seen_save();
    check(!seen_file_has("a1") && seen_file_has("h1") && seen_file_has("z1"), "a1 over: out of the seen list");

    printf("%s: %d of %d cases good\n", good == cases ? "ok" : "FAILED", good, cases);
    return good == cases ? 0 : 1;
}
