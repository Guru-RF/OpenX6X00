/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the S-meter figures the CW and SSB parts of 40 m and 20 m share (#125): the per band S-meter cal
 *  and, on the X6200, the attenuator figure. The real settings (cfg_sm) on a database made from sql/params.sql, with
 *  the rows an earlier version left: 20m SSB calibrated and 20m CW not, as DL9RP's, and the two parts of 40 m with
 *  different values. Bands chosen as a band key does (p_band_id), the figures set as the sliders and Measure set them
 *  (set, and Measure's store), the rows read back after the pending writes are flushed. The cases:
 *  - a part never set takes the other part's row, before the start; a band that shares with none takes the start
 *    (-22 dB on both radios; for the X6200's attenuator 16 dB on the supply, and on battery 59, 34 and 28 dB under
 *    2.5 MHz, to 5 MHz and above);
 *  - the X6200's attenuator figures (#125): the row v0.3.9 and v0.3.10 stored the one figure in is the figure on the
 *    supply, the one on battery starts from its start with no row written; Measure on battery writes the figure on
 *    battery of both parts and leaves the one on the supply as it was, Measure on the supply the other way round,
 *    and a part never set on battery takes the other part's;
 *  - parts holding different values keep their own until one is set; a set writes every part, also when the band
 *    changes to the other part before the writes are flushed, and so does Measure's store at an unchanged value;
 *  - nothing else is shared (the VFO's ATT), and nothing goes to a band outside the parts (30m, Broadcast 41m);
 *  - by frequency: a part renamed still shares, 40m SSB on Region 2's edges too, a third part added within 7.0 to
 *    7.3 MHz shares with both, and a part moved out of 14.0 to 14.35 MHz has its own; Measure's names of the parts.
 *    smeter_share_x6100 / smeter_share_x6200 <params.sql>
 */

#include "cfg/settings_manager.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#define NONE (-999)

static sqlite3 *db;
static int      fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

static void exec(const std::string &q) {
    char *err = nullptr;

    if (sqlite3_exec(db, q.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        printf("SQL failed: %s\n", err ? err : "?");
        exit(2);
    }
}

/* A band's stored value, NONE without a row; the pending writes flushed first, as the flush thread does */
static int row(int band, const char *name) {
    sqlite3_stmt *st;
    int           v = NONE;

    cfg_sm.flush_all();
    sqlite3_prepare_v2(db, "SELECT val FROM band_params WHERE bands_id = ?1 AND name = ?2", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, band);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

static int rows(const char *name) {
    sqlite3_stmt *st;
    int           n = 0;

    cfg_sm.flush_all();
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM band_params WHERE name = ?1", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* As a band key: no flush of its own */
static void to_band(int id) {
    cfg_sm.p_band_id.set(id);
    check(cfg_sm.p_band_id.get() == id, "on the band", id);
}

static int cal() {
    return cfg_sm.p_band_smeter_cal.get();
}

#ifdef BOARD_X6200
static int att() {
    return cfg_sm.p_band_smeter_att.get();
}

static int att_bat() {
    return cfg_sm.p_band_smeter_att_bat.get();
}

/* Measure's end on the X6200 (settings_window.cpp), on battery or on the supply: the figure for that power set, and
 * its row also when unchanged */
static void measure(bool battery, int v) {
    cfg_sm.smeter_att_for(battery).set(v);
    cfg_sm.smeter_att_for(battery).store();
}
#endif

static std::string names(int band) {
    std::string s;

    for (const BandInfo &b : SettingsManager::smeter_parts(band)) s += (s.empty() ? "" : "|") + b.name;
    return s;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: smeter_share_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream sql;

    sql << f.rdbuf();
    check(f.good() && sqlite3_open(":memory:", &db) == SQLITE_OK, "params.sql and the database opened", 0);
    exec(sql.str());

    /* What an earlier version left: 20m SSB calibrated, 20m CW not; 40m CW and SSB calibrated apart */
    exec("INSERT INTO band_params VALUES (7, 'smeter_cal', 9), (3, 'smeter_cal', 12), (4, 'smeter_cal', 13)");
#ifdef BOARD_X6200
    exec("INSERT INTO band_params VALUES (7, 'smeter_att', 30), (3, 'smeter_att', 28), (4, 'smeter_att', 29)");
#endif
    check(cfg_db_init(db), "cfg_db_init", 0);
    cfg_sm.init_load(nullptr);

    /* The starts */
    check(cfg_sm.p_smeter_cal.get() == SMETER_CAL_START, "the cal start, the radio's", cfg_sm.p_smeter_cal.get());
    check(SMETER_CAL_START == -22, "the cal start, -22 on both radios", SMETER_CAL_START);

    /* 20m SSB, the band a new database starts on, has its own; 20m CW takes it, and a load writes nothing */
    check(cfg_sm.p_band_id.get() == 7, "the start on 20m SSB", cfg_sm.p_band_id.get());
    check(cal() == 9, "20m SSB: its own cal", cal());
#ifdef BOARD_X6200
    /* Its attenuator figure of v0.3.10: the one on the supply now; on battery the start, and no row for it */
    check(att() == 30, "20m SSB: the figure of before, on the supply", att());
    check(att_bat() == 28, "20m SSB: on battery the start from 5 MHz up", att_bat());
    check(rows("smeter_att_bat") == 0, "no row on battery from a load", rows("smeter_att_bat"));
#endif
    to_band(6);
    check(cal() == 9, "20m CW never set: 20m SSB's cal", cal());
    check(row(6, "smeter_cal") == NONE, "20m CW: no row from a load", row(6, "smeter_cal"));
#ifdef BOARD_X6200
    check(att() == 30, "20m CW never set: 20m SSB's attenuator", att());
    check(row(6, "smeter_att") == NONE, "20m CW: no attenuator row from a load", row(6, "smeter_att"));
    check(att_bat() == 28, "20m CW, on battery never set: the start from 5 MHz up", att_bat());
#endif

    /* Different values in the two parts of 40 m: each keeps its own */
    to_band(3);
    check(cal() == 12, "40m CW: its own cal", cal());
    to_band(4);
    check(cal() == 13, "40m SSB: its own cal", cal());
#ifdef BOARD_X6200
    check(att() == 29, "40m SSB: its own attenuator", att());
    to_band(3);
    check(att() == 28, "40m CW: its own attenuator", att());
    to_band(4);
#endif

    /* The slider on 40m SSB, then 40m CW before any flush: the write waiting for 40m CW is in */
    cfg_sm.p_band_smeter_cal.set(10);
    to_band(3);
    check(cal() == 10, "40m CW right after a set on 40m SSB", cal());
    check(row(3, "smeter_cal") == 10 && row(4, "smeter_cal") == 10, "both parts' rows", row(3, "smeter_cal"));
#ifdef BOARD_X6200
    cfg_sm.p_band_smeter_att.set(31);
    to_band(4);
    check(att() == 31, "40m SSB right after an attenuator set on 40m CW", att());
    check(row(3, "smeter_att") == 31 && row(4, "smeter_att") == 31, "both parts' attenuator rows",
          row(4, "smeter_att"));

    /* Measure on 20m CW at the value it took from 20m SSB: the rows of both */
    to_band(6);
    measure(false, 30);
    check(row(6, "smeter_att") == 30 && row(7, "smeter_att") == 30, "Measure at an unchanged value, both rows",
          row(6, "smeter_att"));
    measure(false, 33);
    to_band(7);
    check(att() == 33, "20m SSB after Measure on 20m CW", att());

    /* Measure on battery on 40m SSB: the figure on battery of both parts, the one on the supply as it was */
    to_band(4);
    measure(true, 27);
    check(att_bat() == 27 && att() == 31, "40m SSB after Measure on battery: on battery 27, on the supply 31", att());
    check(row(3, "smeter_att_bat") == 27 && row(4, "smeter_att_bat") == 27, "both parts' rows on battery",
          row(3, "smeter_att_bat"));
    check(row(3, "smeter_att") == 31 && row(4, "smeter_att") == 31, "both parts' rows on the supply, as they were",
          row(4, "smeter_att"));
    to_band(3);
    check(att_bat() == 27 && att() == 31, "40m CW: both figures as 40m SSB's", att_bat());

    /* Measure on battery at the start's value: its row all the same */
    to_band(7);
    measure(true, 28);
    check(row(6, "smeter_att_bat") == 28 && row(7, "smeter_att_bat") == 28, "Measure on battery at the start: rows",
          row(7, "smeter_att_bat"));

    /* Measure on the supply after: the figure on the supply, the one on battery as it was */
    measure(false, 16);
    check(att() == 16 && att_bat() == 28, "Measure on the supply: that figure alone", att());
    check(row(6, "smeter_att") == 16 && row(7, "smeter_att") == 16 && row(7, "smeter_att_bat") == 28,
          "the rows on the supply set, on battery as they were", row(7, "smeter_att"));

    /* The slider on battery: as Measure, both parts */
    cfg_sm.p_band_smeter_att_bat.set(26);
    to_band(6);
    check(att_bat() == 26, "20m CW right after a set on battery on 20m SSB", att_bat());

    /* A band never set: its starts, on the supply 16 dB everywhere */
    to_band(1);
    check(att() == 16 && att_bat() == 59, "160m never set: 16 and 59 dB", att_bat());
    to_band(2);
    check(att() == 16 && att_bat() == 34, "80m never set: 16 and 34 dB", att_bat());
    check(row(2, "smeter_att") == NONE && row(2, "smeter_att_bat") == NONE, "80m: no rows from a load",
          row(2, "smeter_att_bat"));
#endif

    /* Nothing else is shared */
    to_band(4);
    cfg_sm.p_band_vfoa_att.set(radio_att_on);
    check(row(4, "vfoa_att") == radio_att_on, "40m SSB: its ATT", row(4, "vfoa_att"));
    check(row(3, "vfoa_att") == NONE, "40m CW: not 40m SSB's ATT", row(3, "vfoa_att"));
    cfg_sm.p_band_vfoa_att.set(radio_att_off);

    /* A band that shares with none: the start, and a set writes it alone */
    int n = rows("smeter_cal");

    to_band(5);
    check(cal() == SMETER_CAL_START, "30m never set: the start", cal());
#ifdef BOARD_X6200
    check(att() == 16, "30m never set: the attenuator start on the supply", att());
    check(att_bat() == 28, "30m never set: the attenuator start on battery", att_bat());
#endif
    cfg_sm.p_band_smeter_cal.set(8);
    check(rows("smeter_cal") == n + 1 && row(5, "smeter_cal") == 8, "30m's set, its row alone", rows("smeter_cal"));
    to_band(103);
    cfg_sm.p_band_smeter_cal.set(5);
    check(row(3, "smeter_cal") == 10 && row(4, "smeter_cal") == 10, "Broadcast 41m's set: 40 m as it was",
          row(3, "smeter_cal"));
    check(names(103) == "Broadcast 41m", "Broadcast 41m: its own name", 103);

    /* By frequency: 40m CW renamed still shares, and Measure names the parts in the list's order */
    check(names(4) == "40m CW|40m SSB", "40m's parts", 4);
    check(names(6) == "20m CW|20m SSB", "20m's parts", 6);
    exec("UPDATE bands SET name = '40m Telegraphy' WHERE id = 3");
    to_band(4);
    cfg_sm.p_band_smeter_cal.set(7);
    check(row(3, "smeter_cal") == 7, "40m CW renamed: still shares", row(3, "smeter_cal"));
    check(names(4) == "40m Telegraphy|40m SSB", "the new name", 4);

    /* 40m SSB on Region 2's edges, to 7.300 */
    exec("UPDATE bands SET stop_freq = 7300000 WHERE id = 4");
    cfg_sm.p_band_smeter_cal.set(6);
    check(row(3, "smeter_cal") == 6, "40m SSB to 7.300: still shares", row(3, "smeter_cal"));

    /* A third part within 7.0 to 7.3 MHz: it takes the first part's row, and its set writes all three */
    exec("UPDATE bands SET stop_freq = 7070000 WHERE id = 4");
    exec("INSERT INTO bands VALUES (400, '40m data', 7070000, 7100000, 1)");
    to_band(400);
    check(cal() == 6, "a third part never set: the others' cal", cal());
    cfg_sm.p_band_smeter_cal.set(4);
    check(row(3, "smeter_cal") == 4 && row(4, "smeter_cal") == 4 && row(400, "smeter_cal") == 4,
          "a third part's set: all three", row(4, "smeter_cal"));
    check(names(400) == "40m Telegraphy|40m SSB|40m data", "the three parts", 400);

    /* 20m CW moved out of 14.0 to 14.35 MHz: its own from then on */
    exec("UPDATE bands SET start_freq = 10000000, stop_freq = 10050000 WHERE id = 6");
    to_band(7);
    cfg_sm.p_band_smeter_cal.set(3);
    check(row(6, "smeter_cal") == NONE, "20m CW moved out: not 20m SSB's set", row(6, "smeter_cal"));
    to_band(6);
    check(cal() == SMETER_CAL_START, "20m CW moved out, never set: the start", cal());
    check(names(6) == "20m CW", "20m CW moved out: its own name", 6);

    /* Outside the bands */
    check(SettingsManager::smeter_parts(BAND_UNDEFINED).empty(), "outside the bands: no parts", BAND_UNDEFINED);

    printf("smeter_share (%s): %d checks, %d failed\n",
#ifdef BOARD_X6200
           "X6200",
#else
           "X6100",
#endif
           checks, fails);
    return fails != 0;
}
