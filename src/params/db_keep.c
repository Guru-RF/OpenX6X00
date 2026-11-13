/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a last good copy of the settings database (db_keep.h)
 */

#include "db_keep.h"

#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUSY_TRIES  20      /* 50 ms apart: the source's WAL being read in by another connection's open */

/* PRAGMA quick_check(1) says ok, and the version row the GUI's migrations keep is there (params.sql starts at 1). The
 * quick check reads every page and its b-tree, not the indexes against their tables (integrity_check, which the
 * start runs on params.db itself, does): a few ms on a radio's file, the whole start's in well under a second */
static bool sound(sqlite3 *db) {
    sqlite3_stmt *st;
    bool          ok = false;

    if (sqlite3_prepare_v2(db, "PRAGMA quick_check(1)", -1, &st, NULL) == SQLITE_OK) {
        const unsigned char *res = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_text(st, 0) : NULL;

        ok = res && !strcmp((const char *)res, "ok");
        sqlite3_finalize(st);
    }
    if (ok) {
        ok = sqlite3_prepare_v2(db, "SELECT id FROM version", -1, &st, NULL) == SQLITE_OK;
        if (ok) {
            ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) >= 1;
            sqlite3_finalize(st);
        }
    }
    return ok;
}

/* The database in rollback mode: SQLite answers with the mode it is in, the one before when it could not change it */
static bool rollback_mode(sqlite3 *db) {
    sqlite3_stmt *st;
    bool          ok = false;

    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode = DELETE", -1, &st, NULL) == SQLITE_OK) {
        const unsigned char *mode = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_text(st, 0) : NULL;

        ok = mode && !strcmp((const char *)mode, "delete");
        sqlite3_finalize(st);
    }
    return ok;
}

static bool fsync_path(const char *path, int flags) {
    int  fd = open(path, flags);
    bool ok = fd >= 0 && fsync(fd) == 0;

    if (fd >= 0) close(fd);
    return ok;
}

/* The folder of path, synced: the rename into it is on the card */
static bool fsync_dir(const char *path) {
    char        dir[PATH_MAX];
    const char *slash = strrchr(path, '/');

    if (!slash) return fsync_path(".", O_RDONLY | O_DIRECTORY);
    if ((size_t)(slash - path) >= sizeof(dir)) return false;
    memcpy(dir, path, slash - path);
    dir[slash - path] = '\0';
    return fsync_path(dir[0] ? dir : "/", O_RDONLY | O_DIRECTORY);
}

/* tmp's own files of SQLite: its rollback journal, and the WAL it has while it is a copy of a WAL database */
static void unlink_sides(const char *tmp) {
    static const char *const side[] = { "-journal", "-wal", "-shm" };
    char                     path[PATH_MAX];

    for (size_t i = 0; i < sizeof(side) / sizeof(side[0]); i++) {
        if (snprintf(path, sizeof(path), "%s%s", tmp, side[i]) < (int)sizeof(path)) unlink(path);
    }
}

bool db_keep_copy(const char *from, const char *to) {
    char            tmp[PATH_MAX];
    sqlite3        *src = NULL, *dst = NULL;
    sqlite3_backup *b;
    bool            ok = false;
    int             rc = SQLITE_ERROR;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", to) >= (int)sizeof(tmp)) {
        return false;
    }
    unlink(tmp);
    unlink_sides(tmp);
    /* Read only: the copy never writes the settings, nor checkpoints their WAL when it closes */
    if (sqlite3_open_v2(from, &src, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_open_v2(tmp, &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        goto out;
    }
    /* Onto the card once, at the end (fsync below), not at each step of the backup */
    sqlite3_exec(dst, "PRAGMA synchronous = OFF", NULL, NULL, NULL);
    b = sqlite3_backup_init(dst, "main", src, "main");
    if (!b) {
        goto out;
    }
    /* All pages in one step, from one read transaction: one moment's settings */
    for (int i = 0; i < BUSY_TRIES; i++) {
        rc = sqlite3_backup_step(b, -1);
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) break;
        sqlite3_sleep(50);
    }
    if (sqlite3_backup_finish(b) != SQLITE_OK || rc != SQLITE_DONE) {
        goto out;
    }
    /* The copy of a WAL database is WAL itself (its header says so): one file in rollback mode instead, that a
     * read-only open and a plain copy onto the DATA partition take as it is */
    ok = rollback_mode(dst) && sound(dst);
out:
    sqlite3_close(src);
    if (dst && sqlite3_close(dst) != SQLITE_OK) ok = false;
    ok = ok && fsync_path(tmp, O_RDONLY) && rename(tmp, to) == 0 && fsync_dir(to);
    if (!ok) {
        unlink(tmp);
        unlink_sides(tmp);
    }
    return ok;
}

bool db_keep_sound(const char *path, time_t *when) {
    struct stat st;
    sqlite3    *db = NULL;
    bool        ok;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
        return false;
    }
    ok = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && sound(db);
    sqlite3_close(db);
    if (ok && when) {
        *when = st.st_mtime;
    }
    return ok;
}
