/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cfg/smeter_att.c (#125): every band of sql/params.sql takes the figure of its group (the bands
 *  written out here, by id, as the manual lists them), outside the bands 40 m's, the groups' edges by the middle of
 *  a band, every figure under the top (SMETER_ATT_MAX) so that Measure can go past it, the deepest attenuator seen
 *  inside the top, and the manual's table ("X6200 bands | Attenuator"), its value outside the bands and its range
 *  ("| Attenuator | ... X6200: 0 to 70 dB") the same as the code's.
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

/* The groups, by the middle of a band: under 5 MHz, to 10, to 20, above. The figures the code must give */
enum { G80, G40, G20, G10, GROUPS };
static const int32_t group_db[GROUPS] = {51, 35, 34, 32};

/* Each band of the list, by id, in the manual's groups */
static const struct {
    int id, group;
} want[] = {
    {1, G80}, {2, G80}, {100, G80}, {101, G80},                 /* 160m, 80m, Broadcast 75m and 60m */
    {13, G40}, {3, G40}, {4, G40}, {102, G40}, {103, G40},      /* 60m, 40m CW and SSB, Broadcast 49m and 41m */
    {104, G40},                                                 /* Broadcast 31m */
    {5, G20}, {6, G20}, {7, G20}, {8, G20}, {105, G20},         /* 30m, 20m CW and SSB, 17m, Broadcast 25m */
    {106, G20}, {107, G20}, {108, G20}, {109, G20},             /* Broadcast 22m to 15m */
    {9, G10}, {10, G10}, {11, G10}, {12, G10}, {110, G10},      /* 15m, 12m, 10m, 6m, Broadcast 13m */
    {111, G10}, {201, G10}, {202, G10}, {203, G10}, {204, G10}, /* Broadcast 11m, CB A to D */
    {300, G10}, {301, G10}, {302, G10},                         /* 4m, 2m, 70cm: a transverter's */
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
    check(smeter_att_start_band(4999999, 4999999) == group_db[G80], "just under 5 MHz: 80 m's", 4999999);
    check(smeter_att_start_band(5000000, 5000000) == group_db[G40], "5 MHz: 40 m's", 5000000);
    check(smeter_att_start_band(4900000, 5100000) == group_db[G40], "a band round 5 MHz, its middle on it", 5000000);
    check(smeter_att_start_band(9999999, 9999999) == group_db[G40], "just under 10 MHz: 40 m's", 9999999);
    check(smeter_att_start_band(10000000, 10000000) == group_db[G20], "10 MHz: 20 m's", 10000000);
    check(smeter_att_start_band(19999999, 19999999) == group_db[G20], "just under 20 MHz: 20 m's", 19999999);
    check(smeter_att_start_band(20000000, 20000000) == group_db[G10], "20 MHz: 10 m's", 20000000);
    check(smeter_att_start_band(999000000, 1000000000) == group_db[G10], "the WR12's top, no overflow", 1000000000);
    check(smeter_att_start_band(135700, 137800) == group_db[G80], "2200 m, a band of your own: 80 m's", 135700);

    bands_of_sql(argv[1]);
    manual_table(argv[2]);

    printf("smeter_att: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
