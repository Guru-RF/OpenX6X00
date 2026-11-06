/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the screensaver's and the screen off's times (src/screen_idle.c). The settings, the uses, TX, the
 *  POWER key and a viewer from afar as screen_light.c gives them, with its tick every second; what comes out is the
 *  stage it then shows. The screensaver after its minutes, the screen dark at the screen off time when there is one
 *  and never when not, the first use on the screensaver or a screen dark by the time only waking it, TX ending the
 *  screensaver and holding it off; without a screensaver, and with the screen always off, the screen off of before.
 *  Then the update (src/params/migrations.c, _16) on a params.db made from sql/params.sql, at the version of the
 *  release before: a radio whose screen off time was changed goes dark at that moment still, with the screensaver
 *  before it; every other radio takes the new defaults.
 *
 *    screen_idle_test <params.sql>
 */

#include "screen_idle.h"
#include "params/migrations.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S   1000ull
#define MIN (60 * S)

sqlite3 *db;    /* migrations.c's (params/db.h) */

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

static screen_idle_t idle(uint8_t saver, uint16_t saver_min, uint16_t off_min, bool always_off) {
    screen_idle_t s = {.saver = saver, .saver_min = saver_min, .off_min = off_min, .always_off = always_off};

    screen_idle_start(&s, 0);
    return s;
}

/* The ticks every second from `from` up to `to`, as screen_light.c's timer: the stage at the end */
static screen_stage_t run(screen_idle_t *s, uint64_t from, uint64_t to) {
    for (uint64_t t = from; t <= to; t += S) {
        screen_idle_tick(s, t);
    }
    return s->stage;
}

/* The stage first reached from `from` on (a second at a time, up to `limit`), and when */
static uint64_t first(screen_idle_t *s, uint64_t from, uint64_t limit, screen_stage_t want) {
    for (uint64_t t = from; t <= limit; t += S) {
        screen_idle_tick(s, t);
        if (s->stage == want) {
            return t;
        }
    }
    return UINT64_MAX;
}

static void times(void) {
    screen_idle_t s;

    /* The defaults: the screensaver after 5 minutes, never dark */
    s = idle(SCREEN_SAVER_DEFAULT, SCREEN_SAVER_MIN_DEFAULT, SCREEN_OFF_MIN_DEFAULT, false);
    check(s.stage == SCREEN_LIT, "start: lit");
    check(run(&s, 0, 5 * MIN - S) == SCREEN_LIT, "defaults: lit for 5 minutes");
    check(run(&s, 5 * MIN, 5 * MIN) == SCREEN_SAVER, "defaults: the screensaver at 5 minutes");
    check(run(&s, 5 * MIN, 12 * 60 * MIN) == SCREEN_SAVER, "defaults: the screensaver 12 hours on, never dark");

    /* The screensaver, then dark */
    s = idle(SCREEN_SAVER_MATRIX, 5, 30, false);
    check(first(&s, 0, 2 * 60 * MIN, SCREEN_SAVER) == 5 * MIN, "saver 5, off 30: the screensaver at 5 minutes");
    check(first(&s, 5 * MIN, 2 * 60 * MIN, SCREEN_DARK) == 30 * MIN, "saver 5, off 30: dark at 30 minutes");
    check(run(&s, 30 * MIN, 10 * 60 * MIN) == SCREEN_DARK, "dark stays dark");
    s = idle(SCREEN_SAVER_MATRIX, 1, 2, false);
    check(first(&s, 0, MIN, SCREEN_SAVER) == MIN, "saver 1, off 2: the screensaver at a minute");
    check(first(&s, MIN, 3 * MIN, SCREEN_DARK) == 2 * MIN, "saver 1, off 2: dark at 2 minutes");
    s = idle(SCREEN_SAVER_MATRIX, 120, 0, false);
    check(first(&s, 0, 3 * 60 * MIN, SCREEN_SAVER) == 120 * MIN, "saver 120: at two hours");

    /* A screen off not later than the screensaver: dark first, no screensaver */
    s = idle(SCREEN_SAVER_MATRIX, 10, 5, false);
    check(first(&s, 0, 60 * MIN, SCREEN_SAVER) == UINT64_MAX, "saver 10, off 5: no screensaver");
    check(s.stage == SCREEN_DARK, "saver 10, off 5: dark");
    s = idle(SCREEN_SAVER_MATRIX, 5, 5, false);
    check(first(&s, 0, 60 * MIN, SCREEN_DARK) == 5 * MIN, "saver 5, off 5: dark at 5 minutes, no screensaver");

    /* A use: the minutes again, and the first one on the screensaver only wakes */
    s = idle(SCREEN_SAVER_MATRIX, 5, 30, false);
    run(&s, 0, 3 * MIN);
    check(!screen_idle_use(&s, 3 * MIN), "a key while lit: not for waking");
    check(first(&s, 3 * MIN, 60 * MIN, SCREEN_SAVER) == 8 * MIN, "a key at 3 minutes: the screensaver at 8");
    check(screen_idle_use(&s, 9 * MIN), "the first key on the screensaver: for waking (swallowed)");
    check(s.stage == SCREEN_LIT, "after it: lit");
    check(!screen_idle_use(&s, 9 * MIN), "the next key: for the radio");
    check(first(&s, 9 * MIN, 60 * MIN, SCREEN_SAVER) == 14 * MIN, "the screensaver 5 minutes after the last key");
    check(first(&s, 14 * MIN, 60 * MIN, SCREEN_DARK) == 39 * MIN, "dark 30 minutes after the last key");
    check(screen_idle_use(&s, 40 * MIN), "the first key on a screen dark by the time: for waking");
    check(s.stage == SCREEN_LIT, "woken: lit");
    check(first(&s, 40 * MIN, 2 * 60 * MIN, SCREEN_SAVER) == 45 * MIN, "woken: the screensaver 5 minutes on");

    /* TX ends the screensaver at once, and holds it off while on the air */
    s = idle(SCREEN_SAVER_MATRIX, 5, 30, false);
    run(&s, 0, 6 * MIN);
    check(s.stage == SCREEN_SAVER, "TX: the screensaver up first");
    screen_idle_tx(&s, true, 6 * MIN);
    check(s.stage == SCREEN_LIT, "TX on: the screensaver gone at once");
    check(run(&s, 6 * MIN, 2 * 60 * MIN) == SCREEN_LIT, "on the air for two hours: no screensaver, not dark");
    screen_idle_tx(&s, false, 2 * 60 * MIN);
    check(first(&s, 2 * 60 * MIN, 3 * 60 * MIN, SCREEN_SAVER) == 125 * MIN, "back on receive: the screensaver 5 on");
    check(first(&s, 125 * MIN, 3 * 60 * MIN, SCREEN_DARK) == 150 * MIN, "and dark 30 minutes after TX");
    screen_idle_tx(&s, true, 151 * MIN);
    check(s.stage == SCREEN_DARK, "TX on a dark screen: dark, as before");
    check(screen_idle_use(&s, 152 * MIN), "a key on it: for waking");
    check(run(&s, 152 * MIN, 200 * MIN) == SCREEN_LIT, "woken while on the air: lit while on the air");
    s = idle(SCREEN_SAVER_MATRIX, 5, 0, false);
    screen_idle_tx(&s, true, 4 * MIN);
    screen_idle_tx(&s, false, 4 * MIN + 30 * S);
    check(first(&s, 4 * MIN + 30 * S, 60 * MIN, SCREEN_SAVER) == 9 * MIN + 30 * S,
          "a short TX: the minutes from its end");

    /* Someone watching from a browser: the screensaver waits, the dark comes at its time */
    s = idle(SCREEN_SAVER_MATRIX, 5, 30, false);
    screen_idle_viewers(&s, true);
    check(first(&s, 0, 29 * MIN, SCREEN_SAVER) == UINT64_MAX, "a viewer: no screensaver");
    check(first(&s, 29 * MIN, 60 * MIN, SCREEN_DARK) == 30 * MIN, "a viewer: dark at 30 minutes still");
    s = idle(SCREEN_SAVER_MATRIX, 5, 0, false);
    run(&s, 0, 6 * MIN);
    screen_idle_viewers(&s, true);
    check(s.stage == SCREEN_LIT, "a viewer comes: the screensaver goes");
    check(!screen_idle_use(&s, 7 * MIN), "the next key: for the radio (nothing to wake)");
    run(&s, 7 * MIN, 20 * MIN);
    screen_idle_viewers(&s, false);
    check(run(&s, 20 * MIN, 20 * MIN) == SCREEN_SAVER, "the viewer gone, the time past: the screensaver");

    /* POWER: dark on purpose, the keys work in the dark */
    s = idle(SCREEN_SAVER_MATRIX, 5, 30, false);
    screen_idle_power(&s, MIN);
    check(s.stage == SCREEN_DARK, "POWER: dark");
    check(!screen_idle_use(&s, 2 * MIN), "a key on the POWER dark screen: for the radio, stays dark");
    check(run(&s, 2 * MIN, 3 * 60 * MIN) == SCREEN_DARK, "POWER dark: hours on, dark");
    screen_idle_power(&s, 3 * 60 * MIN);
    check(s.stage == SCREEN_LIT, "POWER again: lit");
    check(first(&s, 3 * 60 * MIN, 4 * 60 * MIN, SCREEN_SAVER) == 185 * MIN, "then the screensaver 5 minutes on");
    screen_idle_power(&s, 186 * MIN);
    check(s.stage == SCREEN_DARK && s.power_off, "POWER on the screensaver: dark");

    /* The settings changed: applied, the minutes again */
    s = idle(SCREEN_SAVER_MATRIX, 5, 0, false);
    run(&s, 0, 6 * MIN);
    s.saver_min = 10;
    screen_idle_settings(&s, 6 * MIN);
    check(s.stage == SCREEN_LIT, "a setting changed: the screensaver goes");
    check(first(&s, 6 * MIN, 60 * MIN, SCREEN_SAVER) == 16 * MIN, "the new minutes from the change");
    s.saver = SCREEN_SAVER_NONE;
    screen_idle_settings(&s, 17 * MIN);
    check(run(&s, 17 * MIN, 5 * 60 * MIN) == SCREEN_LIT, "set to None, off 0: lit for good");
    s.always_off = true;
    screen_idle_settings(&s, 5 * 60 * MIN);
    check(s.stage == SCREEN_DARK, "always off set: dark at once");
    s.always_off = false;
    screen_idle_settings(&s, 5 * 60 * MIN + S);
    check(s.stage == SCREEN_LIT, "always off cleared: lit");
    s = idle(SCREEN_SAVER_MATRIX, 5, 0, false);
    screen_idle_power(&s, MIN);
    screen_idle_settings(&s, 2 * MIN);
    check(s.stage == SCREEN_DARK, "a setting changed while POWER has it dark: dark");
}

/* Without a screensaver: the screen off of before, exactly */
static void none(void) {
    screen_idle_t s;

    s = idle(SCREEN_SAVER_NONE, 5, 5, false);
    check(first(&s, 0, 60 * MIN, SCREEN_SAVER) == UINT64_MAX, "None: never the screensaver");
    check(s.stage == SCREEN_DARK, "None, off 5: dark");
    s = idle(SCREEN_SAVER_NONE, 5, 5, false);
    check(first(&s, 0, 60 * MIN, SCREEN_DARK) == 5 * MIN, "None, off 5: dark at 5 minutes");
    check(screen_idle_use(&s, 6 * MIN), "None: the key on the dark screen wakes it, swallowed");
    check(first(&s, 6 * MIN, 60 * MIN, SCREEN_DARK) == 11 * MIN, "None: dark 5 minutes after it");
    s = idle(SCREEN_SAVER_NONE, 5, 5, false);
    screen_idle_tx(&s, true, MIN);
    check(first(&s, 0, 60 * MIN, SCREEN_DARK) == 5 * MIN, "None: on the air, dark at 5 minutes as before");
    screen_idle_tx(&s, false, 10 * MIN);
    check(s.stage == SCREEN_DARK, "None: TX ending changes nothing");
    s = idle(SCREEN_SAVER_NONE, 5, 0, false);
    check(run(&s, 0, 24 * 60 * MIN) == SCREEN_LIT, "None, off 0: never dark");
    s = idle(SCREEN_SAVER_NONE, 5, 120, false);
    check(first(&s, 0, 3 * 60 * MIN, SCREEN_DARK) == 120 * MIN, "None, off 120: dark at two hours");
}

/* Always off: dark, the keys in the dark; POWER shows it for the screen off time, a minute at least; no screensaver */
static void always(void) {
    screen_idle_t s;

    s = idle(SCREEN_SAVER_MATRIX, 1, 0, true);
    check(s.stage == SCREEN_DARK, "always off: dark from the start");
    check(!screen_idle_use(&s, MIN), "always off: a key works in the dark, no wake");
    check(run(&s, 0, 60 * MIN) == SCREEN_DARK, "always off: dark, no screensaver");
    screen_idle_power(&s, 60 * MIN);
    check(s.stage == SCREEN_LIT, "always off, POWER: lit");
    screen_idle_use(&s, 60 * MIN + 30 * S);
    check(first(&s, 60 * MIN, 70 * MIN, SCREEN_SAVER) == UINT64_MAX, "always off, shown: no screensaver");
    s = idle(SCREEN_SAVER_MATRIX, 1, 0, true);
    screen_idle_power(&s, 0);
    screen_idle_use(&s, 30 * S);
    check(first(&s, 0, 10 * MIN, SCREEN_DARK) == MIN, "always off, off 0: dark a minute after POWER, keys or not");
    s = idle(SCREEN_SAVER_NONE, 5, 3, true);
    screen_idle_power(&s, 0);
    screen_idle_tx(&s, true, MIN);
    check(first(&s, 0, 10 * MIN, SCREEN_DARK) == 3 * MIN, "always off, off 3: dark 3 minutes after POWER");
    screen_idle_power(&s, 4 * MIN);
    screen_idle_power(&s, 5 * MIN);
    check(s.stage == SCREEN_DARK, "always off: POWER, POWER: dark again");
}

/* ---- The update: _16 on a params.db of the release before ---- */

static char *sql_text;

static int row(const char *name) {
    sqlite3_stmt *st;
    int           v = -1;

    sqlite3_prepare_v2(db, "SELECT val FROM params WHERE name = ?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

/* A new card's params.db (sql/params.sql), at the version `ver`; the screen off time an owner set (-1: none) */
static void radio(int ver, int off_min) {
    char sql[128];

    if (db) {
        sqlite3_close(db);
    }
    sqlite3_open(":memory:", &db);
    check(sqlite3_exec(db, sql_text, NULL, NULL, NULL) == SQLITE_OK, "params.sql makes a params.db");
    snprintf(sql, sizeof(sql), "UPDATE version SET id = %d", ver);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (off_min >= 0) {
        snprintf(sql, sizeof(sql), "INSERT INTO params VALUES ('screen_off_min', %d)", off_min);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    check(migrations_apply() == 0, "the migrations run");
}

/* The settings as params.c loads them: the defaults, and the rows over them */
static screen_idle_t loaded(void) {
    screen_idle_t s = {.saver = SCREEN_SAVER_DEFAULT, .saver_min = SCREEN_SAVER_MIN_DEFAULT,
                       .off_min = SCREEN_OFF_MIN_DEFAULT};
    int v;

    if ((v = row("screen_saver")) >= 0) s.saver = v;
    if ((v = row("screen_saver_min")) >= 0) s.saver_min = v;
    if ((v = row("screen_off_min")) >= 0) s.off_min = v;
    screen_idle_start(&s, 0);
    return s;
}

static void update(void) {
    static const int sets[] = {2, 3, 4, 5, 6, 10, 30, 120};
    screen_idle_t    s;
    char             what[128];

    /* A radio that never set it: the new defaults, the screensaver after 5 minutes and never dark */
    radio(15, -1);
    check(row("screen_saver") < 0 && row("screen_saver_min") < 0, "not set: no rows written");
    s = loaded();
    check(first(&s, 0, 60 * MIN, SCREEN_SAVER) == 5 * MIN, "not set: the screensaver at 5 minutes");
    check(run(&s, 5 * MIN, 24 * 60 * MIN) == SCREEN_SAVER, "not set: never dark");

    /* Set never to go dark: the screensaver, and never dark still */
    radio(15, 0);
    s = loaded();
    check(first(&s, 0, 60 * MIN, SCREEN_SAVER) == 5 * MIN, "set to 0: the screensaver at 5 minutes");
    check(run(&s, 5 * MIN, 24 * 60 * MIN) == SCREEN_SAVER, "set to 0: never dark");

    /* Dark after a minute: no room for a screensaver before it, the screen off of before */
    radio(15, 1);
    check(row("screen_saver") == SCREEN_SAVER_NONE, "set to 1: no screensaver");
    s = loaded();
    check(first(&s, 0, 60 * MIN, SCREEN_DARK) == MIN, "set to 1: dark at a minute");

    /* Set: dark at that moment still, the screensaver before it (5 minutes, or a minute before the dark) */
    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        int      x     = sets[i];
        uint64_t saver = (x <= 5 ? x - 1 : 5) * MIN;

        radio(15, x);
        s = loaded();
        snprintf(what, sizeof(what), "set to %d: the screensaver at %d minutes", x, (int)(saver / MIN));
        check(first(&s, 0, 3 * 60 * MIN, SCREEN_SAVER) == saver, what);
        snprintf(what, sizeof(what), "set to %d: dark at %d minutes, as before", x, x);
        check(first(&s, saver, 3 * 60 * MIN, SCREEN_DARK) == x * MIN, what);
    }

    /* Once, at the version step: a time set afterwards stays as set */
    radio(15, 3);
    sqlite3_exec(db, "UPDATE params SET val = 4 WHERE name = 'screen_off_min'", NULL, NULL, NULL);
    migrations_apply();
    check(row("screen_saver_min") == 2, "applied once: a later change is the owner's");

    /* A new card: the migrations from its first version, nothing of this written */
    radio(1, -1);
    check(row("screen_saver") < 0 && row("screen_saver_min") < 0 && row("screen_off_min") < 0,
          "a new card: the defaults, no rows");
    sqlite3_close(db);
    db = NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: screen_idle_test <params.sql>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    long  n;

    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "%s: not read\n", argv[1]);
        return 2;
    }
    sql_text = calloc(1, n + 1);
    if (fread(sql_text, 1, n, f) != (size_t)n) {
        fprintf(stderr, "%s: not read\n", argv[1]);
        return 2;
    }
    fclose(f);

    times();
    none();
    always();
    update();

    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
