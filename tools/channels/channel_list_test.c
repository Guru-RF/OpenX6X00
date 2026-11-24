/* Host test of the channel list (src/channel_list.c) and of a recall's order (src/channel_recall.h), #346: the GUI's
 * own parser and writer on a channels.json in build/data, with cJSON as the GUI has it.
 *
 * - a new radio's presets: the RTTY, WEFAX and NAVTEX ones written with their app, the others with no "app" key
 * - a file from before the key: read whole, no app anywhere, and written back without the key
 * - "app" as it is read: a known app, a word of a later version (kept and written back), and what is no app (empty,
 *   a number, null, 12 bytes): the channel is kept all the same, and the key gone at the next save
 * - the round trip: read, write, read again gives the same list
 * - Edit held on the radio (channels_app_next, channels_set_app), and the other changes keep a channel's app
 * - a recall: the app first, then frequency, mode, AGC, PRE, ATT; no app for the hand mic's UP/DOWN (with_app false),
 *   for a channel with none and for a word this version does not know
 *
 * With -DOLD_GUI it is built on an older version's src/channel_list.c (make old: v0.3.13's) and says what that
 * version does with a file that has the key: every channel read, and the key gone once it saves.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board.h"
#include "channel_list.h"
#include "cfg/cfg_api.h"
#ifndef OLD_GUI
#include "channel_recall.h"
#endif

#define FILE_PATH BOARD_DATA "/channels.json"

/* The radio's settings, for Add */
struct ComputedParamInt { int32_t v; };
static ComputedParamInt freq = {14074000}, mode = {radio_mode_usb}, agc = {radio_agc_auto}, pre = {0}, att = {1};
ComputedParamInt *cfg_fg_freq = &freq, *cfg_cur_mode = &mode, *cfg_cur_agc = &agc, *cfg_cur_pre = &pre,
                 *cfg_cur_att = &att;

int32_t cparam_i_get(const ComputedParamInt *p) {
    return p->v;
}

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void put_file(const char *text) {
    FILE *f = fopen(FILE_PATH, "w");

    if (!f || fputs(text, f) == EOF || fclose(f) != 0) {
        perror(FILE_PATH);
        exit(2);
    }
}

static char *get_file(void) {
    static char buf[65536];
    FILE       *f = fopen(FILE_PATH, "r");
    size_t      n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;

    if (f) fclose(f);
    buf[n] = '\0';
    return buf;
}

static int count(const char *text, const char *word) {
    int n = 0;

    for (const char *p = strstr(text, word); p; p = strstr(p + 1, word)) n++;
    return n;
}

#define ENTRY(name, hz, more) \
    "{\"name\":\"" name "\",\"frequency\":" #hz ",\"mode\":\"USB-D\",\"agc\":\"SLOW\",\"pre\":false,\"att\":true" more "}"

/* A file with the key in every shape */
static const char with_apps[] =
    "{\"version\":1,\"channels\":["
    ENTRY("PLAIN", 7074000, "") ","
    ENTRY("RTTY DWD", 4582200, ",\"app\":\"RTTY\"") ","
    ENTRY("FAX", 3853500, ",\"app\":\"WEFAX\"") ","
    ENTRY("NAV", 517000, ",\"app\":\"NAVTEX\"") ","
    ENTRY("PICTURES", 14230000, ",\"app\":\"SSTV\"") ","
    ENTRY("LATER", 3580000, ",\"app\":\"HELL\"") ","
    ENTRY("EMPTY", 7050000, ",\"app\":\"\"") ","
    ENTRY("NUMBER", 7060000, ",\"app\":5") ","
    ENTRY("NULL", 7070000, ",\"app\":null") ","
    ENTRY("TWELVE", 7080000, ",\"app\":\"TWELVE BYTES\"") ","
    ENTRY("ELEVEN", 7090000, ",\"app\":\"ELEVEN BYTE\"")
    "]}\n";

#ifdef OLD_GUI

int main(void) {
    put_file(with_apps);
    check(channels_load(), "an older version reads a file with the key");
    check(channels_count() == 11, "and keeps every channel of it");
    check(!strcmp(channels_get(1)->name, "RTTY DWD") && channels_get(1)->freq == 4582200, "as it reads any other");
    check(count(get_file(), "\"app\"") == 10, "reading it leaves the file as it is");
    check(channels_set_name(0, "RENAMED") && count(get_file(), "\"app\"") == 0,
          "a change made in that version's Channels window writes the list without the key");
    check(count(get_file(), "\"name\"") == 11, "with every channel in it");
    printf("OLD GUI: %d checks, %d failed: a file with \"app\" is read whole; its own save drops the key\n", checks,
           failures);
    return failures != 0;
}

#else

static char seen[256];

static void op_app(const char *app) { snprintf(seen + strlen(seen), sizeof(seen) - strlen(seen), "app=%s ", app); }
static void op_freq(uint32_t hz) { snprintf(seen + strlen(seen), sizeof(seen) - strlen(seen), "freq=%u ", hz); }
static void op_mode(radio_mode_t m) {
    snprintf(seen + strlen(seen), sizeof(seen) - strlen(seen), "mode=%s ", channels_mode_name(m));
}
static void op_agc(radio_agc_t a) {
    snprintf(seen + strlen(seen), sizeof(seen) - strlen(seen), "agc=%s ", channels_agc_name(a));
}
static void op_pre(bool on) { snprintf(seen + strlen(seen), sizeof(seen) - strlen(seen), "pre=%d ", on); }
static void op_att(bool on) { snprintf(seen + strlen(seen), sizeof(seen) - strlen(seen), "att=%d", on); }

static const channel_recall_ops_t ops = {op_app, op_freq, op_mode, op_agc, op_pre, op_att};

static const char *recall(uint16_t i, bool with_app, bool *opened) {
    channel_t c = *channels_get(i);

    seen[0] = '\0';
    *opened = channel_recall(&c, with_app, &ops);
    return seen;
}

int main(void) {
    static channel_t before[CHANNELS_MAX];
    const char      *text;
    uint16_t         n;
    bool             opened;

    /* a new radio: the presets */
    unlink(FILE_PATH);
    check(channels_load(), "no file: the presets are written");
    n    = channels_count();
    text = get_file();
    check(n >= 10 && count(text, "\"name\"") == n, "one entry a preset");
    {
        int with = 0, right = 1;

        for (uint16_t i = 0; i < n; i++) {
            const channel_t *c    = channels_get(i);
            const char      *want = !strncmp(c->name, "RTTY ", 5)     ? "RTTY"
                                    : !strncmp(c->name, "WEFAX ", 6)  ? "WEFAX"
                                    : !strncmp(c->name, "NAVTEX ", 7) ? "NAVTEX"
                                                                      : "";

            right = right && !strcmp(c->app, want);
            with += c->app[0] != 0;
        }
        check(right, "the RTTY, WEFAX and NAVTEX presets name their app, the others none");
        check(with == 14 && count(text, "\"app\"") == with, "\"app\" is written for those alone");
        check(count(text, "\"app\":\t\"RTTY\"") == 5 && count(text, "\"app\":\t\"WEFAX\"") == 7 &&
              count(text, "\"app\":\t\"NAVTEX\"") == 2, "5 RTTY, 7 WEFAX, 2 NAVTEX");
    }
    check(channels_load() && channels_count() == n, "the presets' file is read back whole");

    /* a file from before the key */
    put_file("{\"version\":1,\"channels\":[" ENTRY("NAVTEX MF", 517000, "") "," ENTRY("RTTY DWD DDK2", 4582200, "") ","
             ENTRY("40M NET", 7150000, "") "]}\n");
    check(channels_load() && channels_count() == 3, "a file with no \"app\" anywhere: read whole");
    check(!channels_get(0)->app[0] && !channels_get(1)->app[0] && !channels_get(2)->app[0],
          "its channels open no app, whatever their names: the file is not changed for them");
    check(channels_get(1)->freq == 4582200 && channels_get(1)->mode == radio_mode_usb_dig &&
          channels_get(1)->agc == radio_agc_slow && !channels_get(1)->pre && channels_get(1)->att,
          "and are what they were");
    check(channels_save() && count(get_file(), "\"app\"") == 0 && count(get_file(), "\"name\"") == 3,
          "written back without the key");

    /* the key in every shape */
    put_file(with_apps);
    check(channels_load() && channels_count() == 11, "\"app\" of any shape leaves no channel out");
    check(!channels_get(0)->app[0], "no key: no app");
    check(!strcmp(channels_get(1)->app, "RTTY") && !strcmp(channels_get(2)->app, "WEFAX") &&
          !strcmp(channels_get(3)->app, "NAVTEX") && !strcmp(channels_get(4)->app, "SSTV"), "the four apps");
    check(!strcmp(channels_get(5)->app, "HELL") && !channels_app_known("HELL"),
          "a word this version does not know is kept as it is");
    check(!channels_get(6)->app[0] && !channels_get(7)->app[0] && !channels_get(8)->app[0] &&
          !channels_get(9)->app[0], "empty, a number, null, 12 bytes: no app");
    check(!strcmp(channels_get(10)->app, "ELEVEN BYTE"), "11 bytes fit");
    check(channels_get(9)->freq == 7080000 && !strcmp(channels_get(9)->name, "TWELVE"), "the channel itself is whole");

    /* the round trip */
    for (uint16_t i = 0; i < channels_count(); i++) before[i] = *channels_get(i);
    check(channels_save(), "saved");
    text = get_file();
    check(count(text, "\"app\"") == 6, "\"app\" written for the six that have one, also the unknown word");
    check(count(text, "\"HELL\"") == 1 && count(text, "TWELVE BYTES") == 0, "the unknown word kept, the misfit gone");
    check(channels_load() && channels_count() == 11, "read again");
    {
        int same = 1;

        for (uint16_t i = 0; i < 11; i++) {
            const channel_t *c = channels_get(i);

            same = same && !strcmp(c->name, before[i].name) && c->freq == before[i].freq &&
                   c->mode == before[i].mode && c->agc == before[i].agc && c->pre == before[i].pre &&
                   c->att == before[i].att && !strcmp(c->app, before[i].app);
        }
        check(same, "the same list after write and read");
    }

    /* the apps, and Edit held */
    check(!strcmp(channels_app_id(0), "RTTY") && !strcmp(channels_app_id(1), "WEFAX") &&
          !strcmp(channels_app_id(2), "NAVTEX") && !strcmp(channels_app_id(3), "SSTV") && !channels_app_id(4),
          "RTTY, WEFAX, NAVTEX, SSTV");
    check(channels_app_known("RTTY") && channels_app_known("SSTV") && !channels_app_known("") &&
          !channels_app_known("rtty") && !channels_app_known(NULL), "known by their exact word");
    check(!strcmp(channels_app_next(""), "RTTY") && !strcmp(channels_app_next("RTTY"), "WEFAX") &&
          !strcmp(channels_app_next("WEFAX"), "NAVTEX") && !strcmp(channels_app_next("NAVTEX"), "SSTV") &&
          !strcmp(channels_app_next("SSTV"), "") && !strcmp(channels_app_next("HELL"), "RTTY"),
          "Edit held goes none, RTTY, WEFAX, NAVTEX, SSTV, none; from an unknown word to RTTY");
    check(channels_set_app(0, "NAVTEX") && !strcmp(channels_get(0)->app, "NAVTEX") &&
          count(get_file(), "\"app\"") == 7, "an app set is saved at once");
    check(channels_set_app(0, "") && !channels_get(0)->app[0] && count(get_file(), "\"app\"") == 6,
          "and taken away, the key gone");
    check(channels_set_app(0, NULL) && !channels_set_app(0, "TWELVE BYTES") && !channels_set_app(99, "RTTY") &&
          !channels_get(0)->app[0], "12 bytes and a channel that is not there: refused");
    check(channels_set_name(1, "DDK2") && !strcmp(channels_get(1)->app, "RTTY"), "a rename keeps the app");
    check(channels_move(1, 3) && !strcmp(channels_get(3)->name, "DDK2") && !strcmp(channels_get(3)->app, "RTTY") &&
          !strcmp(channels_get(1)->app, "WEFAX") && channels_save(), "a move carries it");
    check(channels_delete(1) && !strcmp(channels_get(1)->app, "NAVTEX") && channels_count() == 10,
          "a delete takes it with its channel");
    check(channels_add_current() && channels_count() == 11 && !channels_get(10)->app[0] &&
          channels_get(10)->freq == 14074000 && channels_get(10)->att, "Add: the radio's settings, no app");
    check(channels_load() && !strcmp(channels_get(2)->app, "RTTY") && !channels_get(10)->app[0],
          "all of it on the card");

    /* a recall: 2 is DDK2 (RTTY), 0 PLAIN (none), 4 LATER (HELL) */
    check(!strcmp(recall(2, true, &opened), "app=RTTY freq=4582200 mode=USB-D agc=SLOW pre=0 att=1") && opened,
          "a recall from the window: the app first, then frequency, mode, AGC, PRE, ATT");
    check(!strcmp(recall(2, false, &opened), "freq=4582200 mode=USB-D agc=SLOW pre=0 att=1") && !opened,
          "the hand mic's UP/DOWN: tuned, no app");
    check(!strcmp(recall(0, true, &opened), "freq=7074000 mode=USB-D agc=SLOW pre=0 att=1") && !opened,
          "a channel with no app: tuned as before");
    check(!strcmp(channels_get(4)->app, "HELL") &&
          !strcmp(recall(4, true, &opened), "freq=3580000 mode=USB-D agc=SLOW pre=0 att=1") && !opened,
          "a word this version does not know: tuned, nothing opened");
    check(!strcmp(recall(1, true, &opened), "app=NAVTEX freq=517000 mode=USB-D agc=SLOW pre=0 att=1") && opened,
          "NAVTEX likewise");

    unlink(FILE_PATH);
    printf("%s: %d checks, %d failed\n", failures ? "CHANNEL LIST FAILED" : "CHANNEL LIST OK", checks, failures);
    return failures != 0;
}

#endif
