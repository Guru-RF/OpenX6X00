/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cfg/smeter_att.c (#125): every band of sql/params.sql takes the figure of its group (the bands
 *  written out here, by id, as the manual lists them), outside the bands 40 m's, the groups' edges by the middle of
 *  a band, every figure under the top (SMETER_ATT_MAX) so that Measure can go past it, the deepest attenuator seen
 *  inside the top, and the manual's table ("X6200 bands | Attenuator"), its value outside the bands, its range
 *  ("| Attenuator | ... X6200: 0 to 70 dB") and Measure's line at the top ("ATT 70, the top, so 70 or more") the
 *  same as the code's. And the S-meter cal a band never calibrated starts at, on either radio, the same in the
 *  manual ("A band never calibrated starts at -22 dB on the X6100 and +11 dB on the X6200") as in smeter_att.h.
 *    smeter_att_test <params.sql> <MANUAL.md>
 */

#include "cfg/smeter_att.h"

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

/* The groups, by the middle of a band: under 2.5 MHz, to 5, above. The figures the code must give */
enum { G160, G80, G40, GROUPS };
static const int32_t group_db[GROUPS] = {59, 51, 34};

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
            check(smeter_att_start_band(start, stop) == group_db[want[i].group], "a band's figure, its group's", id);
        }
        if (strchr(line, ';')) break;   /* the last row */
    }
    fclose(f);
    check(rows == WANT_N, "as many bands as this test lists", rows);
    for (int i = 0; i < WANT_N; i++) {
        check(seen[i] == 1, "a band of this test's list once in params.sql", want[i].id);
    }
}

/* The manual's table: a "| ... | 51 dB |" row for each group, in order, then "which starts at 35 dB" outside them.
 * Before it the settings' table, the attenuator's range: "| Attenuator | 0 to 40 dB (X6200: 0 to 70 dB, ..." */
static void manual_table(const char *path) {
    FILE *f = fopen(path, "r");
    char  line[512];
    int   row = -2, outside = -1, top = -1;

    check(f != NULL, "MANUAL.md opened", 0);
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *range = strstr(line, "| Attenuator | 0 to ");

        if (range && (range = strstr(range, "X6200: 0 to "))) top = atoi(range + strlen("X6200: 0 to "));
        if (row == -2) {
            if (strstr(line, "| X6200 bands | Attenuator |")) row = -1;     /* then the |---| line */
            continue;
        }
        if (row == -1) {
            row = 0;
            continue;
        }
        if (row < GROUPS) {
            char *bar = strrchr(line, '|');
            int   db  = -1;

            if (bar) *bar = '\0';
            bar = strrchr(line, '|');
            if (bar) sscanf(bar, "| %d dB", &db);
            check(db == group_db[row], "the manual's figure, a group's", db);
            row++;
            continue;
        }
        char *s = strstr(line, "which starts at ");

        if (s) {
            outside = atoi(s + strlen("which starts at "));
            break;
        }
    }
    fclose(f);
    check(row == GROUPS, "the manual's table, a row for each group", row);
    check(outside == smeter_att_start_band(0, 0), "the manual's value outside the bands", outside);
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

/* The manual's S-meter cal start, its sentence across lines: "... starts at -22 dB on the X6100 and +11 dB on the
 * X6200" */
static void manual_cal_start(const char *path) {
    static char text[1 << 20];
    FILE       *f = fopen(path, "r");
    size_t      n;
    int         x6100 = 99, x6200 = 99;

    if (!f) return;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    for (char *c = text; *c; c++) {
        if (*c == '\n') *c = ' ';
    }
    char *s = strstr(text, "A band never calibrated starts at ");

    check(s != NULL, "the manual says where a band never calibrated starts", 0);
    if (!s) return;
    sscanf(s + strlen("A band never calibrated starts at "), "%d dB on the X6100 and %d dB on the X6200", &x6100,
           &x6200);
    check(x6100 == SMETER_CAL_START_X6100, "the manual's cal start on the X6100, the code's", x6100);
    check(x6200 == SMETER_CAL_START_X6200, "the manual's cal start on the X6200, the code's", x6200);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: smeter_att_test <params.sql> <MANUAL.md>\n");
        return 2;
    }
    for (int g = 0; g < GROUPS; g++) {
        check(group_db[g] > 0 && group_db[g] < SMETER_ATT_MAX, "a start under the top, Measure can go past it",
              group_db[g]);
    }
    check(DEEPEST_DB + DEEPEST_SPREAD <= SMETER_ATT_MAX, "the deepest attenuator seen, inside the top", DEEPEST_DB);
    check(smeter_att_start_band(0, 0) == group_db[G40], "outside the bands: 40 m's", smeter_att_start_band(0, 0));

    /* The edges, by the middle */
    check(smeter_att_start_band(2499999, 2499999) == group_db[G160], "just under 2.5 MHz: 160 m's", 2499999);
    check(smeter_att_start_band(2500000, 2500000) == group_db[G80], "2.5 MHz: 80 m's", 2500000);
    check(smeter_att_start_band(2400000, 2600000) == group_db[G80], "a band round 2.5 MHz, its middle on it", 2500000);
    check(smeter_att_start_band(4999999, 4999999) == group_db[G80], "just under 5 MHz: 80 m's", 4999999);
    check(smeter_att_start_band(5000000, 5000000) == group_db[G40], "5 MHz: 40 m's", 5000000);
    check(smeter_att_start_band(4900000, 5100000) == group_db[G40], "a band round 5 MHz, its middle on it", 5000000);
    check(smeter_att_start_band(999000000, 1000000000) == group_db[G40], "the WR12's top, no overflow", 1000000000);
    check(smeter_att_start_band(135700, 137800) == group_db[G160], "2200 m, a band of your own: 160 m's", 135700);

    bands_of_sql(argv[1]);
    manual_table(argv[2]);
    manual_measure_top(argv[2]);
    manual_cal_start(argv[2]);

    printf("smeter_att: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
