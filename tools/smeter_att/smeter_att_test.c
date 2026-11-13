/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cfg/smeter_att.c (#125): every band of sql/params.sql takes the figures of its group (the bands
 *  written out here, by id, as the manual lists them), on the supply 16 dB on every band and on battery by group,
 *  outside the bands 40 m's, the groups' edges by the middle of a band, every figure under the top (SMETER_ATT_MAX)
 *  so that Measure can go past it, the deepest attenuator seen inside the top, and the manual's table ("X6200 bands
 *  | On the supply | On battery"), its values outside the bands, its range ("| Attenuator | ... X6200: 0 to 70 dB")
 *  and Measure's line at the top ("ATT 70, the top, so 70 or more") the same as the code's. And the S-meter cal a
 *  band never calibrated starts at, one for both radios, the same in the manual ("A band never calibrated starts at
 *  -22 dB on both radios") as in smeter_att.h.
 *  And which figure the meter takes (smeter_att_now) as the supply comes and goes, the X6200's readings fed to
 *  bat_watch_reading and bat_supply_reading as radio.c feeds them: the figure on the supply until the readings count
 *  after power-on, the one on battery 5 s after they say battery, back to the supply's 5 s after it is plugged in,
 *  neither changed by a supply that flickers in or out for less, a power-on on the supply never on battery, and a
 *  change the GUI's queue had no room for told again at the next reading. The rows: the figure on the supply keeps
 *  the row of the one figure of v0.3.9 and v0.3.10, so a value stored then is read as the figure on the supply, and
 *  the one on battery has its own (tools/smeter_share loads and stores them in the settings).
 *  And the bands that share both (smeter_share_group): 40m CW and SSB, 20m CW and SSB, no other band of the list,
 *  40m SSB on Region 2's edges too, the ranges' edges by the middle of a band, and the manual's ranges ("from 7.000 to
 *  7.300 MHz share one value, and those from 14.000 to 14.350 MHz another") the code's.
 *    smeter_att_test <params.sql> <MANUAL.md>
 */

#include "cfg/smeter_att.h"
#include "bat_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

static void check(int ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* The deepest an X6200's attenuator was seen to go, dB, and how far that reading wandered: 80 m on the other radio,
 * 4 November (#125). The top has to hold it */
#define DEEPEST_DB 62
#define DEEPEST_SPREAD 2

/* The groups, by the middle of a band: under 2.5 MHz, to 5, above. The figures the code must give, on the supply and
 * on battery */
enum { G160, G80, G40, GROUPS };
static const int32_t supply_db[GROUPS]  = {16, 16, 16};
static const int32_t battery_db[GROUPS] = {59, 34, 28};

/* Each band of the list, by id, in the manual's groups. G40: 40 m's figure, from 60 m up */
static const struct {
    int id, group;
} want[] = {
    {1, G160},                                                  /* 160m */
    {2, G80}, {100, G80}, {101, G80},                           /* 80m, Broadcast 75m and 60m */
    {13, G40}, {3, G40}, {4, G40}, {102, G40}, {103, G40},      /* 60m, 40m CW and SSB, Broadcast 49m and 41m */
    {104, G40},                                                 /* Broadcast 31m */
    {5, G40}, {6, G40}, {7, G40}, {8, G40}, {105, G40},         /* 30m, 20m CW and SSB, 17m, Broadcast 25m */
    {106, G40}, {107, G40}, {108, G40}, {109, G40},             /* Broadcast 22m to 15m */
    {9, G40}, {10, G40}, {11, G40}, {12, G40}, {110, G40},      /* 15m, 12m, 10m, 6m, Broadcast 13m */
    {111, G40}, {201, G40}, {202, G40}, {203, G40}, {204, G40}, /* Broadcast 11m, CB A to D */
    {300, G40}, {301, G40}, {302, G40},                         /* 4m, 2m, 70cm: a transverter's */
};

#define WANT_N ((int)(sizeof(want) / sizeof(want[0])))

/* The bands of the list that share the S-meter figures, by id, with their group; every other band shares none */
static const struct {
    int id, group;
} shared[] = {{3, 40}, {4, 40}, {6, 20}, {7, 20}};

static int shared_group(int id) {
    for (size_t i = 0; i < sizeof(shared) / sizeof(shared[0]); i++) {
        if (shared[i].id == id) return shared[i].group;
    }
    return 0;
}

/* The rows of "INSERT INTO bands VALUES": (id, 'name', start, stop, type) */
static void bands_of_sql(const char *path) {
    FILE *f = fopen(path, "r");
    char  line[256], name[64];
    int   in = 0, rows = 0, seen[WANT_N] = {0};

    check(f != NULL, "params.sql opened", 0);
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned start, stop;
        int      id, type;

        if (strstr(line, "INSERT INTO bands VALUES")) {
            in = 1;
            continue;
        }
        if (!in) continue;
        if (sscanf(line, " (%d, '%63[^']', %u, %u, %d)", &id, name, &start, &stop, &type) != 5) break;
        rows++;
        int i = 0;

        while (i < WANT_N && want[i].id != id) i++;
        check(i < WANT_N, "a band of params.sql in this test's list (add it)", id);
        if (i < WANT_N) {
            seen[i]++;
            check(smeter_att_start_band(start, stop, false) == supply_db[want[i].group],
                  "a band's figure on the supply, its group's", id);
            check(smeter_att_start_band(start, stop, true) == battery_db[want[i].group],
                  "a band's figure on battery, its group's", id);
        }
        check(smeter_share_group(start, stop) == shared_group(id), "a band's share, 40m and 20m CW and SSB only", id);
        if (strchr(line, ';')) break;   /* the last row */
    }
    fclose(f);
    check(rows == WANT_N, "as many bands as this test lists", rows);
    for (int i = 0; i < WANT_N; i++) {
        check(seen[i] == 1, "a band of this test's list once in params.sql", want[i].id);
    }
}

/* The manual's table: a "| ... | 16 dB | 34 dB |" row for each group, in order. Before it the settings' table, the
 * attenuator's range: "| Attenuator | 0 to 40 dB (X6200: 0 to 70 dB, ..." */
static void manual_table(const char *path) {
    FILE *f = fopen(path, "r");
    char  line[512];
    int   row = -2, top = -1;

    check(f != NULL, "MANUAL.md opened", 0);
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *range = strstr(line, "| Attenuator | 0 to ");

        if (range && (range = strstr(range, "X6200: 0 to "))) top = atoi(range + strlen("X6200: 0 to "));
        if (row == -2) {
            if (strstr(line, "| X6200 bands | On the supply | On battery |")) row = -1;     /* then the |---| line */
            continue;
        }
        if (row == -1) {
            row = 0;
            continue;
        }
        if (row < GROUPS) {
            char *bar = strrchr(line, '|');
            int   db[2] = {-1, -1};

            /* The last two cells, from the right: on battery, then on the supply */
            for (int c = 1; c >= 0 && bar; c--) {
                *bar = '\0';
                bar  = strrchr(line, '|');
                if (bar) sscanf(bar, "| %d dB", &db[c]);
            }
            check(db[0] == supply_db[row], "the manual's figure on the supply, a group's", db[0]);
            check(db[1] == battery_db[row], "the manual's figure on battery, a group's", db[1]);
            row++;
            continue;
        }
        break;
    }
    fclose(f);
    check(row == GROUPS, "the manual's table, a row for each group", row);
    check(top == SMETER_ATT_MAX, "the manual's range, the code's top", top);
}

/* Measure at the top, as the manual quotes its line (settings_window.cpp): "... ATT 70, the top, so 70 or more." */
static void manual_measure_top(const char *path) {
    FILE *f = fopen(path, "r");
    char  line[512];
    int   found = 0;

    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        for (char *s = strstr(line, "ATT "); s; s = strstr(s + 1, "ATT ")) {
            int at, more;

            if (sscanf(s, "ATT %d, the top, so %d or more", &at, &more) == 2) {
                found++;
                check(at == SMETER_ATT_MAX && more == SMETER_ATT_MAX, "the manual's Measure at the top, the code's",
                      at);
            }
        }
    }
    fclose(f);
    check(found > 0, "the manual quotes Measure at the top", found);
}

/* The manual as one line, each run of spaces and line ends one space, for a sentence across lines (a list item's go
 * on indented) */
static char *manual_text(const char *path) {
    static char text[1 << 20];
    FILE       *f = fopen(path, "r");
    size_t      n, o = 0;

    if (!f) return NULL;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) {
        int space = text[i] == ' ' || text[i] == '\n';

        if (!space || o == 0 || text[o - 1] != ' ') text[o++] = space ? ' ' : text[i];
    }
    text[o] = '\0';
    return text;
}

/* After the manual's table, outside the bands, its sentence across lines: "... which starts at 16 dB on the supply
 * and 28 dB on battery" */
static void manual_outside(const char *path) {
    char *text = manual_text(path);
    int   supply = -1, battery = -1;

    if (!text) return;
    char *s = strstr(text, "| X6200 bands | On the supply | On battery |");

    if (s) s = strstr(s, "which starts at ");
    check(s != NULL, "the manual says where outside the bands starts", 0);
    if (!s) return;
    sscanf(s, "which starts at %d dB on the supply and %d dB on battery", &supply, &battery);
    check(supply == smeter_att_start_band(0, 0, false), "the manual's value outside the bands on the supply", supply);
    check(battery == smeter_att_start_band(0, 0, true), "the manual's value outside the bands on battery", battery);
}

/* The manual's S-meter cal start, its sentence across lines: "... starts at -22 dB on both radios" */
static void manual_cal_start(const char *path) {
    char *text = manual_text(path);
    int   start = 99, end = 0;

    if (!text) return;
    char *s = strstr(text, "A band never calibrated starts at ");

    check(s != NULL, "the manual says where a band never calibrated starts", 0);
    if (!s) return;
    sscanf(s + strlen("A band never calibrated starts at "), "%d dB on both radios%n", &start, &end);
    check(end > 0, "the manual's cal start, one for both radios", 0);
    check(start == SMETER_CAL_START, "the manual's cal start, the code's", start);
}

/* The manual's ranges of the bands that share, their sentence across lines: "... the bands whose middle lies from
 * 7.000 to 7.300 MHz share one value, and those from 14.000 to 14.350 MHz another." Each range's edges by the middle
 * of a band, against the code's */
static void manual_share(const char *path) {
    char     *text = manual_text(path);
    double    mhz[4] = {0};
    const int group[2] = {40, 20};

    if (!text) return;
    char *s = strstr(text, "the bands whose middle lies from ");

    check(s != NULL, "the manual says which bands share", 0);
    if (!s) return;
    check(sscanf(s + strlen("the bands whose middle lies from "),
                 "%lf to %lf MHz share one value, and those from %lf to %lf MHz another", &mhz[0], &mhz[1], &mhz[2],
                 &mhz[3]) == 4,
          "the manual's two ranges", 0);
    for (int r = 0; r < 2; r++) {
        uint32_t lo = (uint32_t)(mhz[2 * r] * 1e6 + 0.5), hi = (uint32_t)(mhz[2 * r + 1] * 1e6 + 0.5);

        check(smeter_share_group(lo, lo) == group[r], "the manual's range, from its start", lo);
        check(smeter_share_group(lo - 1, lo - 1) == 0, "the manual's range, nothing under it", lo);
        check(smeter_share_group(hi - 1, hi - 1) == group[r], "the manual's range, up to its end", hi);
        check(smeter_share_group(hi, hi) == 0, "the manual's range, nothing from its end", hi);
    }
}

/* The meter's figure as the supply comes and goes, on the X6200: a reading every 0.4 s as radio.c takes them
 * (bat_watch_reading, then bat_supply_reading, whose state supply_cb gives the meter), and the figure smeter.c adds
 * back with ATT on (smeter_att_now) from a band's two, here 16 dB on the supply and 30 on battery */
#define READ_MS     400
#define STEADY_MS   ((BAT_SUPPLY_MS + READ_MS - 1) / READ_MS * READ_MS)    /* the first reading 5 s on */
#define T0          1000000
#define ON_SUPPLY   16
#define ON_BATTERY  30

static bat_watch_t  w;
static bat_supply_t sup;
static uint64_t     now;

static void power_on(void) {
    memset(&w, 0, sizeof(w));
    memset(&sup, 0, sizeof(sup));
    now = T0;
}

/* Readings for ms, on the battery alone (8.3 V, nothing on DC IN) or with a 13.5 V supply in, the base's external
 * power and charging flags with it as the X6200's raises them. The figure the meter takes after them; *changed: when
 * it first changed, ms into the stretch, or -1 */
static int32_t feed(bool supply, long ms, long *changed) {
    *changed = -1;
    for (long t = 0; t < ms; t += READ_MS, now += READ_MS) {
        bat_watch_reading(&w, 83, supply ? 135 : 0, supply, supply, false, false, now);
        if (bat_supply_reading(&sup, &w, now) && *changed < 0) *changed = t;
    }
    return smeter_att_now(ON_SUPPLY, ON_BATTERY, sup.bat);
}

static void supply_switch(void) {
    long    at;
    int32_t db;

    check(smeter_att_now(ON_SUPPLY, ON_BATTERY, false) == ON_SUPPLY, "on the supply: the figure on the supply", 0);
    check(smeter_att_now(ON_SUPPLY, ON_BATTERY, true) == ON_BATTERY, "on battery: the figure on battery", 0);

    /* Switched on on battery: the figure on the supply until the readings count (BAT_SETTLE_MS), as the TX power cap
     * assumes external power then, and the one on battery BAT_SUPPLY_MS after */
    power_on();
    db = feed(false, BAT_SETTLE_MS, &at);
    check(db == ON_SUPPLY && at < 0, "power-on on battery, before the readings count: the supply's", at);
    db = feed(false, 2 * BAT_SUPPLY_MS, &at);
    check(db == ON_BATTERY, "then on battery: the battery's", db);
    check(at == STEADY_MS, "the battery's once the readings have said so for 5 s", at);

    /* The supply plugged in: the battery's for 5 s, then the supply's */
    db = feed(true, STEADY_MS - READ_MS, &at);
    check(db == ON_BATTERY && at < 0, "the supply in, under 5 s: still the battery's", at);
    db = feed(true, BAT_SUPPLY_MS, &at);
    check(db == ON_SUPPLY && at == READ_MS, "the supply in for 5 s: the supply's, from the reading 5 s on", at);

    /* A flicker out and back: no change either way */
    db = feed(false, 2000, &at);
    check(db == ON_SUPPLY && at < 0, "the supply out for 2 s: still the supply's", at);
    db = feed(true, 2 * BAT_SUPPLY_MS, &at);
    check(db == ON_SUPPLY && at < 0, "and back: the supply's throughout", at);

    /* Unplugged: the battery's 5 s on */
    db = feed(false, 2 * BAT_SUPPLY_MS, &at);
    check(db == ON_BATTERY && at == STEADY_MS, "unplugged: the battery's after 5 s", at);

    /* A flicker in and out, just under 5 s: no change */
    db = feed(true, STEADY_MS - READ_MS, &at);
    check(db == ON_BATTERY && at < 0, "a supply in for just under 5 s: the battery's", at);
    db = feed(false, 2 * BAT_SUPPLY_MS, &at);
    check(db == ON_BATTERY && at < 0, "and out again: the battery's throughout", at);

    /* The GUI's queue full: radio.c takes the change back and it comes again at the next reading */
    db = feed(true, STEADY_MS + READ_MS, &at);
    check(db == ON_SUPPLY && at == STEADY_MS, "plugged in: the change 5 s on", at);
    sup.bat = !sup.bat;     /* scheduler_put refused it */
    db = feed(true, READ_MS, &at);
    check(db == ON_SUPPLY && at == 0, "a change not told: again at the next reading", at);

    /* Switched on on the supply: the supply's from the start, never the battery's */
    power_on();
    db = feed(true, BAT_SETTLE_MS + 3 * BAT_SUPPLY_MS, &at);
    check(db == ON_SUPPLY && at < 0, "power-on on the supply: the supply's throughout", at);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: smeter_att_test <params.sql> <MANUAL.md>\n");
        return 2;
    }
    for (int g = 0; g < GROUPS; g++) {
        check(supply_db[g] > 0 && supply_db[g] < SMETER_ATT_MAX, "a start on the supply under the top", supply_db[g]);
        check(battery_db[g] > 0 && battery_db[g] < SMETER_ATT_MAX, "a start on battery under the top", battery_db[g]);
        /* The attenuator took more off on battery on every band read on both (#125) */
        check(battery_db[g] > supply_db[g], "a start on battery over the one on the supply", battery_db[g]);
    }
    check(DEEPEST_DB + DEEPEST_SPREAD <= SMETER_ATT_MAX, "the deepest attenuator seen, inside the top", DEEPEST_DB);
    check(smeter_att_start_band(0, 0, true) == battery_db[G40], "outside the bands on battery: 40 m's",
          smeter_att_start_band(0, 0, true));
    check(smeter_att_start_band(0, 0, false) == supply_db[G40], "outside the bands on the supply: 40 m's",
          smeter_att_start_band(0, 0, false));

    /* The edges, by the middle, on battery; on the supply one figure throughout */
    check(smeter_att_start_band(2499999, 2499999, true) == battery_db[G160], "just under 2.5 MHz: 160 m's", 2499999);
    check(smeter_att_start_band(2500000, 2500000, true) == battery_db[G80], "2.5 MHz: 80 m's", 2500000);
    check(smeter_att_start_band(2400000, 2600000, true) == battery_db[G80], "a band round 2.5 MHz, its middle on it",
          2500000);
    check(smeter_att_start_band(4999999, 4999999, true) == battery_db[G80], "just under 5 MHz: 80 m's", 4999999);
    check(smeter_att_start_band(5000000, 5000000, true) == battery_db[G40], "5 MHz: 40 m's", 5000000);
    check(smeter_att_start_band(4900000, 5100000, true) == battery_db[G40], "a band round 5 MHz, its middle on it",
          5000000);
    check(smeter_att_start_band(999000000, 1000000000, true) == battery_db[G40], "the WR12's top, no overflow",
          1000000000);
    check(smeter_att_start_band(135700, 137800, true) == battery_db[G160], "2200 m, a band of your own: 160 m's",
          135700);
    check(smeter_att_start_band(135700, 137800, false) == supply_db[G160], "2200 m on the supply", 135700);
    check(smeter_att_start_band(999000000, 1000000000, false) == supply_db[G40], "the WR12's top on the supply",
          1000000000);

    /* The rows: the figure on the supply reads the row v0.3.9 and v0.3.10 stored the one figure in, so a value
     * measured or set there is the one on the supply; the one on battery has a row of its own */
    check(strcmp(SMETER_ATT_ROW, "smeter_att") == 0, "the figure on the supply, the row of before", 0);
    check(strcmp(SMETER_ATT_BAT_ROW, SMETER_ATT_ROW) != 0, "the figure on battery, a row of its own", 0);

    supply_switch();

    /* The bands that share */
    check(smeter_share_group(7040000, 7300000) == 40, "40m SSB on Region 2's edges: 40 m's", 7300000);
    check(smeter_share_group(7200000, 7500000) == 0, "Broadcast 41m: none", 7200000);
    check(smeter_share_group(7000000, 7200000) == 40, "40m in one band: 40 m's", 7000000);
    check(smeter_share_group(0, 0) == 0, "outside the bands: none", 0);
    check(smeter_share_group(10100000, 10150000) == 0, "a part moved to 30 m: none", 10100000);

    bands_of_sql(argv[1]);
    manual_table(argv[2]);
    manual_measure_top(argv[2]);
    manual_outside(argv[2]);
    manual_cal_start(argv[2]);
    manual_share(argv[2]);

    printf("smeter_att: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
