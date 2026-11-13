/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the settings' last good copy (src/params/db_keep.c) and what the GUI's start does with it
 *  (src/params/db.c, database_init). The real code on params.db files made from sql/params.sql in build/data, the
 *  copy in build/sys (the system partition's /var/lib/x6100 on a radio). Each case is a start of its own (a child
 *  process: database_init keeps its state in the file's statics).
 *
 *  - A clean start, then the copy: one file in rollback mode (no -wal beside it), sound, with what was committed
 *    through the GUI's connection and still in its WAL, and nothing of a transaction that connection has open.
 *  - A damaged params.db (pages overwritten, its header gone, cut to nothing) with a copy: put aside, the copy in its
 *    place, the note "Settings restored from <date time>\nthe settings file was damaged" in the copy's local time,
 *    the start trace's line; the copy is made again of it.
 *  - Without a copy, with a damaged one, with one of another program (no version row): the defaults, as before.
 *  - No params.db (a factory reset, which restarts the GUI without S95gui): the defaults, never the copy, no note,
 *    nothing put aside, a stale WAL of the old file gone.
 *  - A start on the defaults in /tmp (this version cannot use the GUI's own file): no copy made, the one there kept.
 *  - A copy of a damaged file never replaces the good one; database_keep_drop takes it away.
 *  - How long the checks take on a params.db of a radio used for years (10,000 ATU rows, 100 memories): printed.
 *
 *    settings_keep_test <params.sql>
 */

#define _GNU_SOURCE

#include "params/db.h"
#include "params/db_keep.h"

#include <dirent.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define DATA    BOARD_DATA      /* BUILD/data */
#define PARAMS  DATA "/params.db"
#define DEFAULT BOARD_SHARE "/params.default.db"
#define KEEP    PARAMS_KEEP
#define KEPT_AT 1794561600      /* 2026-11-13 09:20 UTC, 10:20 in the test's zone (CET) */

char start_trace_last[512];     /* stubs.c: the start trace's last line */

static struct { int cases, bad; } *count;   /* shared with the cases' processes */
static FILE *report;    /* the test's lines: a case's start prints its own (the migrations', db.c's), VERBOSE=1 shows them */

static void check(bool ok, const char *what) {
    count->cases++;
    if (!ok) {
        fprintf(report, "WRONG %s\n", what);
        count->bad++;
    }
}

static bool exists(const char *path) {
    struct stat st;

    return stat(path, &st) == 0;
}

static long mtime(const char *path) {
    struct stat st;

    return stat(path, &st) == 0 ? (long)st.st_mtime : -1;
}

static void sh(const char *cmd) {
    if (system(cmd) != 0) {
        fprintf(report, "FAILED %s\n", cmd);
        exit(2);
    }
}

static bool exec_on(const char *path, const char *sql) {
    sqlite3 *d;
    bool     ok = sqlite3_open(path, &d) == SQLITE_OK && sqlite3_exec(d, sql, NULL, NULL, NULL) == SQLITE_OK;

    sqlite3_close(d);
    return ok;
}

/* params.val of name in the file, -1 when not there */
static int value_in(sqlite3 *d, const char *name) {
    sqlite3_stmt *st;
    int           v = -1;

    if (sqlite3_prepare_v2(d, "SELECT val FROM params WHERE name = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

static int value_at(const char *path, const char *name) {
    sqlite3 *d;
    int      v = -1;

    if (sqlite3_open_v2(path, &d, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) v = value_in(d, name);
    sqlite3_close(d);
    return v;
}

/* Bytes at an offset of the file overwritten (a power cut in the middle of a write), or the file cut to `cut` */
static void damage(const char *path, long at, long n, long cut) {
    int  fd = open(path, O_WRONLY);
    char junk[4096];

    memset(junk, 0x55, sizeof(junk));
    if (cut >= 0) {
        check(ftruncate(fd, cut) == 0, "damage: cut");
    }
    for (long done = 0; done < n; done += sizeof(junk)) {
        check(pwrite(fd, junk, sizeof(junk), at + done) == (ssize_t)sizeof(junk), "damage: written");
    }
    close(fd);
}

/* Files of the data folder whose names begin with prefix */
static int count_files(const char *prefix) {
    DIR           *d = opendir(DATA);
    struct dirent *e;
    int            n = 0;

    while (d && (e = readdir(d))) {
        if (!strncmp(e->d_name, prefix, strlen(prefix))) n++;
    }
    if (d) closedir(d);
    return n;
}

/* A card as a radio leaves it: params.db from the defaults with a setting of the owner's, nothing else. With kept, the
 * last good copy of it, made KEPT_AT; the owner's setting changed after it */
static void card(bool kept) {
    sh("rm -rf " DATA " " BUILD "/sys " BUILD "/tmp && mkdir -p " DATA " " BUILD "/sys " BUILD "/tmp && cp " DEFAULT " "
       PARAMS);
    check(exec_on(PARAMS, "INSERT INTO params VALUES ('owner', 1)"), "card: the owner's setting");
    if (kept) {
        struct utimbuf t = { KEPT_AT, KEPT_AT };

        check(db_keep_copy(PARAMS, KEEP), "card: the copy made");
        check(utime(KEEP, &t) == 0, "card: the copy's time");
        check(exec_on(PARAMS, "UPDATE params SET val = 2 WHERE name = 'owner'"), "card: changed after the copy");
    }
}

/* One case, a start of its own */
static void run(const char *name, void (*fn)(void)) {
    int   before = count->bad;
    pid_t pid;
    int   status;

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        report = fdopen(dup(1), "w");
        if (!getenv("VERBOSE") && (!freopen("/dev/null", "w", stdout) || !freopen("/dev/null", "w", stderr))) {
            _exit(2);
        }
        fn();
        fflush(report);
        _exit(0);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("WRONG %s: the start ended with status %d\n", name, status);
        count->bad++;
    }
    printf("%-58s %s\n", name, count->bad == before ? "ok" : "WRONG");
}

static void clean_start(void) {
    struct stat st;
    char        head[20];
    int         fd;
    sqlite3    *d;

    card(false);
    check(database_init(), "clean: the settings open");
    check(!database_note(), "clean: nothing to say");
    check(value_in(db, "owner") == 1, "clean: the owner's setting");
    /* Written through the GUI's connection: one committed, still in the WAL; one in a transaction left open */
    check(sqlite3_exec(db, "INSERT INTO params VALUES ('committed', 7)", NULL, NULL, NULL) == SQLITE_OK, "clean: write");
    check(stat(PARAMS "-wal", &st) == 0 && st.st_size > 0, "clean: the write in the WAL");
    check(sqlite3_exec(db, "BEGIN; INSERT INTO params VALUES ('open', 8)", NULL, NULL, NULL) == SQLITE_OK,
          "clean: a transaction open");
    check(database_keep(), "clean: the copy made");
    check(!exists(KEEP ".tmp") && !exists(KEEP "-wal") && !exists(KEEP "-shm") && !exists(KEEP "-journal") &&
          !exists(KEEP ".tmp-journal") && !exists(KEEP ".tmp-wal"), "clean: one file, nothing beside it");
    fd = open(KEEP, O_RDONLY);
    check(fd >= 0 && read(fd, head, sizeof(head)) == sizeof(head) && head[18] == 1 && head[19] == 1,
          "clean: the copy in rollback mode (its header)");
    if (fd >= 0) close(fd);
    check(db_keep_sound(KEEP, NULL), "clean: the copy sound");
    check(value_at(KEEP, "committed") == 7, "clean: the copy has what the WAL had");
    check(value_at(KEEP, "open") == -1, "clean: nothing of the open transaction");
    check(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK, "clean: commit");
    check(database_keep(), "clean: the copy again");
    check(value_at(KEEP, "open") == 8, "clean: now with it");
    check(sqlite3_open_v2(KEEP, &d, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK, "clean: the copy opens read-only");
    check(value_in(d, "owner") == 1, "clean: read from it");
    sqlite3_close(d);
    check(!exists(KEEP "-wal") && !exists(KEEP "-shm"), "clean: a read leaves nothing beside it");
    check(count_files("params-unusable") == 0, "clean: nothing put aside");
}

static void restored_check(const char *what) {
    char line[160];

    check(database_init(), "restored: the settings open");
    snprintf(line, sizeof(line), "%s: the note", what);
    check(database_note() && !strcmp(database_note(),
                                     "Settings restored from 2026-11-13 10:20\nthe settings file was damaged"), line);
    if (database_note() && strcmp(database_note(), "Settings restored from 2026-11-13 10:20\nthe settings file was damaged"))
        fprintf(report, "  note: %s\n", database_note());
    snprintf(line, sizeof(line), "%s: the copy's settings", what);
    check(value_in(db, "owner") == 1, line);
    snprintf(line, sizeof(line), "%s: the damaged file put aside", what);
    check(count_files("params-unusable-") == 1, line);
    snprintf(line, sizeof(line), "%s: the start trace", what);
    check(strstr(start_trace_last, "put aside as params-unusable-") &&
          strstr(start_trace_last, "the last good copy of 2026-11-13 10:20 loaded"), line);
    snprintf(line, sizeof(line), "%s: the copy made again of it", what);
    check(database_keep() && mtime(KEEP) != KEPT_AT && value_at(KEEP, "owner") == 1, line);
    check(!database_version_fault(), "restored: no fault of the version");
}

static void damaged_pages(void) {
    card(true);
    damage(PARAMS, 4096, 16384, -1);
    restored_check("pages overwritten");
}

static void damaged_header(void) {
    card(true);
    damage(PARAMS, 0, 4096, -1);
    restored_check("header gone");
}

static void damaged_empty(void) {
    card(true);
    damage(PARAMS, 0, 0, 0);
    restored_check("cut to nothing");
}

static void defaults_check(const char *what) {
    char line[160];

    check(database_init(), "defaults: the settings open");
    snprintf(line, sizeof(line), "%s: the defaults' note", what);
    check(database_note() && !strcmp(database_note(), "Settings file unusable: put aside, defaults loaded"), line);
    snprintf(line, sizeof(line), "%s: the defaults", what);
    check(value_in(db, "owner") == -1, line);
    snprintf(line, sizeof(line), "%s: put aside", what);
    check(count_files("params-unusable-") == 1, line);
}

static void no_copy(void) {
    card(false);
    damage(PARAMS, 4096, 16384, -1);
    defaults_check("no copy");
}

static void damaged_copy(void) {
    card(true);
    damage(PARAMS, 4096, 16384, -1);
    damage(KEEP, 4096, 16384, -1);
    check(!db_keep_sound(KEEP, NULL), "damaged copy: not sound");
    defaults_check("damaged copy");
}

static void foreign_copy(void) {
    card(true);
    check(exec_on(KEEP, "DELETE FROM version"), "foreign copy: no version row");
    check(!db_keep_sound(KEEP, NULL), "foreign copy: not the GUI's");
    damage(PARAMS, 0, 4096, -1);
    defaults_check("foreign copy");
}

static void missing(void) {
    int fd;

    card(true);
    sh("rm -f " PARAMS);
    fd = open(PARAMS "-wal", O_WRONLY | O_CREAT, 0644);     /* a WAL left of the file that went */
    check(fd >= 0 && write(fd, "stale", 5) == 5, "missing: a stale WAL");
    if (fd >= 0) close(fd);
    check(database_init(), "missing: the settings open");
    check(!database_note(), "missing: nothing to say");
    check(value_in(db, "owner") == -1, "missing: the defaults, not the copy");
    check(count_files("params-unusable") == 0, "missing: nothing put aside");
    check(mtime(KEEP) == KEPT_AT, "missing: the copy left as it is");
}

static void spare(void) {
    card(true);
    /* The GUI's own file (version 1), which this version cannot bring up to date: its ATU table gone */
    check(exec_on(PARAMS, "DROP TABLE atu"), "spare: a file this version cannot migrate");
    check(database_init(), "spare: a start on the defaults");
    check(database_version_fault(), "spare: the version's fault");
    check(!database_keep(), "spare: no copy of the defaults in /tmp");
    database_keep_later();
    usleep(200000);
    check(mtime(KEEP) == KEPT_AT, "spare: the copy there kept");
    check(exists(PARAMS) && count_files("params-unusable") == 0, "spare: params.db left as it is");
}

static void bad_source(void) {
    card(true);
    damage(PARAMS, 4096, 16384, -1);
    check(!db_keep_copy(PARAMS, KEEP), "bad source: no copy");
    check(mtime(KEEP) == KEPT_AT && db_keep_sound(KEEP, NULL), "bad source: the good copy kept");
    check(!exists(KEEP ".tmp") && !exists(KEEP ".tmp-journal"), "bad source: nothing left of the try");
    check(!db_keep_copy(DATA "/none.db", KEEP) && mtime(KEEP) == KEPT_AT, "no source: the good copy kept");
}

static void drop(void) {
    card(true);
    check(database_init(), "drop: the settings open");
    database_keep_drop();
    check(!exists(KEEP), "drop: the copy gone");
}

static void later(void) {
    card(false);
    check(database_init(), "later: the settings open");
    database_keep_later();
    for (int i = 0; i < 50 && !exists(KEEP); i++) usleep(20000);
    check(db_keep_sound(KEEP, NULL), "later: the copy made in its thread");
}

static double ms_since(const struct timespec *t0) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1e3 + (t.tv_nsec - t0->tv_nsec) / 1e6;
}

/* The checks' time on params.db of a radio used for years: the ATU's tune of every 500 Hz of HF on two antennas, 100
 * memories, many band settings */
static void timing(void) {
    struct timespec t0;
    struct stat     st;
    double          init, keep, quick;

    card(false);
    check(exec_on(PARAMS,
                  "BEGIN;"
                  "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n WHERE i < 4999) "
                  "INSERT INTO atu SELECT a, 1800000 + i * 500, i FROM n, (SELECT 0 AS a UNION SELECT 1);"
                  "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n WHERE i < 999) "
                  "INSERT INTO memory SELECT i / 10, 'p' || (i % 10), i FROM n;"
                  "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n WHERE i < 1999) "
                  "INSERT INTO band_params SELECT 1 + i % 34, 'q' || i, i FROM n;"
                  "COMMIT;"), "timing: a large params.db");
    stat(PARAMS, &st);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    check(database_init(), "timing: the settings open");
    init = ms_since(&t0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    check(database_keep(), "timing: the copy made");
    keep = ms_since(&t0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 10; i++) check(db_keep_sound(KEEP, NULL), "timing: the copy sound");
    quick = ms_since(&t0) / 10;
    fprintf(report, "  params.db of %ld kB: the start's open, integrity check and migrations %.1f ms, the copy %.1f ms, "
           "its quick_check %.1f ms\n", (long)st.st_size / 1024, init, keep, quick);
    check(init < 1000 && keep < 1000 && quick < 1000, "timing: each well under a second on a PC");
}

int main(int argc, char **argv) {
    FILE    *f;
    char    *sql;
    long     n;
    sqlite3 *d;

    if (argc != 2 || !(f = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: settings_keep_test <params.sql>\n");
        return 2;
    }
    report = stdout;
    setenv("TZ", "CET-1", 1);
    tzset();
    count = mmap(NULL, sizeof(*count), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    sql = calloc(1, n + 1);
    if (fread(sql, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    /* The defaults as the image has them (sql/CMakeLists.txt) */
    sh("rm -rf " BOARD_SHARE " && mkdir -p " BOARD_SHARE);
    check(sqlite3_open(DEFAULT, &d) == SQLITE_OK && sqlite3_exec(d, sql, NULL, NULL, NULL) == SQLITE_OK,
          "params.sql makes the defaults");
    sqlite3_close(d);
    free(sql);

    run("a clean start, then its copy", clean_start);
    run("params.db's pages overwritten: the copy", damaged_pages);
    run("params.db's header gone: the copy", damaged_header);
    run("params.db cut to nothing: the copy", damaged_empty);
    run("damaged, no copy: the defaults", no_copy);
    run("damaged, the copy damaged too: the defaults", damaged_copy);
    run("damaged, a copy not the GUI's: the defaults", foreign_copy);
    run("no params.db (a factory reset): the defaults, not the copy", missing);
    run("a start on the defaults in /tmp: no copy", spare);
    run("a copy of a damaged file: the good one kept", bad_source);
    run("the copy dropped (factory reset)", drop);
    run("the copy in a thread of its own", later);
    run("the checks' time on a large params.db", timing);

    printf("%d checks, %d wrong\n", count->cases, count->bad);
    return count->bad ? 1 : 0;
}
