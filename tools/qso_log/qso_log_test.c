/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the radio's QSO list, qso_log.db (src/qso_log.c), with another writer on the file, as the web
 *  configurator's Logs page is one now (#284: a QSO deleted there goes from the list too, logs_follow_test.py).
 *  - A worked search that found the call on this band and mode stopped at that row and left its query open: the
 *    file stayed read-locked until the next search, for good once the app was closed, and the page's delete could
 *    not write. Each search lets go of the file now, and the next one sees the delete: no restart.
 *  - A QSO saved while the page writes waits for it (the busy timeout), where it failed with "database is locked".
 *  The file is the GUI's own, in build/data (BOARD_DATA).
 */

#include "qso_log.h"
#include "params/params.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

params_t params;

void msg_update_text_fmt(const char *fmt, ...) {
    (void)fmt;
}

int adif_read(const char *path, qso_log_record_t **records, int *skipped) {
    (void)path, (void)records, (void)skipped;
    return -1;
}

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

/* 2026-11-04 18:02:59 UTC */
static const time_t T = 1793815379;

/* The other writer: the web configurator, here with no busy timeout of its own, so a lock it meets is an error */
static sqlite3 *page(void) {
    sqlite3 *db = NULL;

    if (sqlite3_open(BOARD_DATA "/qso_log.db", &db) != SQLITE_OK)
        return NULL;
    sqlite3_busy_timeout(db, 0);
    return db;
}

static int exec(sqlite3 *db, const char *sql) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);

    if (rc != SQLITE_OK)
        printf("  %s: %s\n", sql, sqlite3_errmsg(db));
    return rc;
}

/* The page deletes a QSO right after the FT8 list found its call worked on this band and mode */
static void delete_after_search(void) {
    qso_log_record_t r = qso_log_record_create("ON6URE", "EA5KDD", T, MODE_FT8, -6, 2, 7074000, NULL, NULL, "JO21",
                                               "IN81");
    sqlite3 *db = page();

    check(qso_log_record_save(r) == 1, "the QSO not saved");
    check(qso_log_search_worked("EA5KDD", MODE_FT8, BAND_40M) == SEARCH_WORKED_SAME_MODE,
          "EA5KDD not worked on 40 m FT8");
    check(qso_log_search_worked("EA5KDD", MODE_FT8, BAND_20M) == SEARCH_WORKED_YES, "EA5KDD not worked elsewhere");
    check(qso_log_search_worked("EA5KDD", MODE_FT8, BAND_40M) == SEARCH_WORKED_SAME_MODE,
          "EA5KDD not worked on 40 m FT8, a second time");
    check(db && exec(db, "DELETE FROM qso_log WHERE remote_callsign = 'EA5KDD'") == SQLITE_OK,
          "the page's delete refused after a search (the list's read lock)");
    check(qso_log_search_worked("EA5KDD", MODE_FT8, BAND_40M) == SEARCH_WORKED_NO,
          "EA5KDD still worked after the page deleted it");
    sqlite3_close(db);
}

/* The page holds the file for a moment (a delete of a whole season) while the FT8 app saves a QSO */
static void *page_writes(void *arg) {
    sqlite3 *db = arg;

    usleep(300 * 1000);
    exec(db, "COMMIT");
    return NULL;
}

static void save_while_page_writes(void) {
    qso_log_record_t r  = qso_log_record_create("ON6URE", "DL1ABC", T + 75, MODE_FT4, -12, -9, 14080000, NULL, NULL,
                                                "JO21", "JO62");
    sqlite3         *db = page();
    pthread_t        thr;

    check(db && exec(db, "BEGIN EXCLUSIVE") == SQLITE_OK, "the page's write not begun");
    pthread_create(&thr, NULL, page_writes, db);
    check(qso_log_record_save(r) == 1, "a QSO saved while the page writes: lost");
    pthread_join(thr, NULL);
    check(qso_log_search_worked("DL1ABC", MODE_FT4, BAND_20M) == SEARCH_WORKED_SAME_MODE,
          "DL1ABC not worked on 20 m FT4");
    sqlite3_close(db);
}

/* A compound call is worked by its base call, as the page writes it (apps.py canon_call) */
static void compound(void) {
    qso_log_record_t r = qso_log_record_create("ON6URE", "PA/G4ABC/P", T + 140, MODE_FT8, 0, 0, 50313000, NULL,
                                               NULL, "JO21", "");

    check(qso_log_record_save(r) == 1, "PA/G4ABC/P not saved");
    check(qso_log_search_worked("G4ABC", MODE_FT8, BAND_6M) == SEARCH_WORKED_SAME_MODE, "G4ABC not worked on 6 m");
}

/* The FT8 app's countries worked read each call of the log once, as logged (any band or mode) */
static void count_call(const char *call, void *ud) {
    char *seen = ud;

    if (strlen(seen) + strlen(call) + 2 < 256) {
        strcat(seen, call);
        strcat(seen, " ");
    }
}

static void each_call(void) {
    char seen[256] = "";
    int  n;

    check(qso_log_record_save(qso_log_record_create("ON6URE", "DL1ABC", T + 200, MODE_FT8, 0, 0, 7074000, NULL, NULL,
                                                    "JO21", "")) == 1,
          "DL1ABC on 40 m FT8 not saved");
    n = qso_log_each_call(count_call, seen);
    check(n == 2, "each call once: DL1ABC (twice in the log) and PA/G4ABC/P");
    check(strstr(seen, "DL1ABC ") && strstr(seen, "PA/G4ABC/P "), "the calls as logged");
    check(!strstr(seen, "EA5KDD"), "a QSO the page deleted still read");
}

int main(void) {
    unlink(BOARD_DATA "/qso_log.db");
    if (!qso_log_init()) {
        printf("qso_log.db not opened in " BOARD_DATA "\n");
        return 1;
    }
    delete_after_search();
    save_while_page_writes();
    compound();
    each_call();
    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
