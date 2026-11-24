/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the hidden apps (src/cfg/apps_list.c, #351).
 *
 *  The apps: every one has a name for the setting and one for the screen, no two the same, the setting's in small
 *  letters and digits only (a comma or a space would split it). The setting: "" hides nothing; names by commas or
 *  spaces, in any order, twice, with spaces around; a name that is not known is left out and the others kept (an
 *  app of a later version, a typing error); "settings" is never taken; the text written back is in the apps' order
 *  and reads back as the same set, whole names only when the room is short. An app added later: a setting written
 *  before it existed does not name it, so it is shown. Hiding and showing one app leaves the others as they were.
 *  The pages: the APP bar's 23 apps on six pages of four as the manual has them; with apps hidden the others close
 *  up in the same order on fewer pages, no gap before the last page's end; all but Settings hidden leaves one page
 *  with Settings; a page past the last has the last one's apps.
 *
 *  The sources are read too: the APP grid's table (src/apps_menu.c) names every app once, and the bar's (src/
 *  button_bar.cpp) the 23 of this test's order once and no other. The web Display page has no list of its own: it
 *  shows the radio's (tools/apps_web).
 *
 *    apps_hide_test <src dir>
 */

#include "apps_list.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

static uint32_t bit(int id) {
    return 1u << id;
}

/* The APP button pages' order, as button_bar.cpp's app_btns[] and the manual's table */
static const uint8_t bar[] = {
    APPID_MODES,       APPID_CLUSTER,   APPID_SWRSCAN,    APPID_RECORDER,
    APPID_SETTINGS,    APPID_CWTRAINER, APPID_FIELD,      APPID_CHANNELS,
    APPID_BANDS,       APPID_GPS,       APPID_SCREEN_REC, APPID_CW_KBD,
    APPID_PROPAGATION, APPID_SCAN,      APPID_IRADIO,     APPID_ZELLO,
    APPID_SVX,         APPID_FLIGHTS,   APPID_AERO,       APPID_CB,
    APPID_METEO,       APPID_MAIL,      APPID_DMR,
};

#define BAR_N ((int)(sizeof(bar) / sizeof(bar[0])))

static int at(uint32_t hidden, int page, int slot) {
    return apps_page_app(bar, BAR_N, hidden, APPS_PER_PAGE, page, slot);
}

/* A source file whole, NULL when it is not there */
static char *slurp(const char *dir, const char *name) {
    char  path[512];
    FILE *f;
    char *text;
    long  size;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "r"))) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    text = calloc(1, (size_t)size + 1);
    if (fread(text, 1, (size_t)size, f) != (size_t)size) text[0] = '\0';
    fclose(f);
    return text;
}

/* How often word stands in text as a whole identifier */
static int count(const char *text, const char *word) {
    int    n = 0;
    size_t l = strlen(word);

    for (const char *p = text; (p = strstr(p, word)); p += l) {
        if (!isalnum((unsigned char)p[l]) && p[l] != '_') n++;
    }
    return n;
}

static void sources(const char *src) {
    static const char *ids[APPID_COUNT] = {
        "APPID_MODES",   "APPID_CLUSTER",   "APPID_FIELD",      "APPID_BANDS",  "APPID_PROPAGATION",
        "APPID_CHANNELS", "APPID_SCAN",     "APPID_SWRSCAN",    "APPID_RECORDER", "APPID_CWTRAINER",
        "APPID_CW_KBD",  "APPID_MAIL",      "APPID_GPS",        "APPID_SCREEN_REC", "APPID_WALKIE",
        "APPID_BIRDIE",  "APPID_IRADIO",    "APPID_ZELLO",      "APPID_SVX",    "APPID_DMR",
        "APPID_FLIGHTS", "APPID_AERO",      "APPID_CB",         "APPID_METEO",  "APPID_SETTINGS",
    };
    char *grid = slurp(src, "apps_menu.c"), *bar_src = slurp(src, "button_bar.cpp");
    char  what[96];

    check(grid && bar_src, "src/apps_menu.c or src/button_bar.cpp is not read");
    if (!grid || !bar_src) return;
    for (int id = 0; id < APPID_COUNT; id++) {
        bool on_bar = false;

        for (int i = 0; i < BAR_N; i++) on_bar |= bar[i] == id;
        snprintf(what, sizeof(what), "%s is not once in the APP grid's table", ids[id]);
        check(count(grid, ids[id]) == 1, what);
        snprintf(what, sizeof(what), "%s on the bar: button_bar.cpp and this test differ", ids[id]);
        check(count(bar_src, ids[id]) == (on_bar ? 1 : 0), what);
    }
    free(grid);
    free(bar_src);
}

int main(int argc, char **argv) {
    char     text[APPS_HIDDEN_LEN], small[16];
    uint8_t  out[APPID_COUNT];
    uint32_t hidden, all = 0;

    /* The apps */
    for (int id = 0; id < APPID_COUNT; id++) {
        const char *key = apps_list_key(id), *name = apps_list_name(id);
        bool        plain = key && key[0];

        check(key && name && name[0], "an app without a name");
        for (const char *p = key; p && *p; p++) plain &= (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
        check(plain, "a setting name that is not small letters and digits");
        for (int o = 0; o < id; o++) {
            check(strcmp(key, apps_list_key(o)) != 0, "two apps with one setting name");
            check(strcmp(name, apps_list_name(o)) != 0, "two apps with one name");
        }
        check(apps_list_can_hide(id) == (id != APPID_SETTINGS), "which apps can be hidden");
        if (apps_list_can_hide(id)) all |= bit(id);
    }
    check(!apps_list_key(-1) && !apps_list_key(APPID_COUNT) && !apps_list_name(APPID_COUNT), "an id that is none has a name");
    check(!apps_list_can_hide(-1) && !apps_list_can_hide(APPID_COUNT), "an id that is none can be hidden");
    check(!strcmp(apps_list_key(APPID_ZELLO), "zello") && !strcmp(apps_list_key(APPID_SETTINGS), "settings"),
          "the setting names of Zello and Settings changed: a radio's setting would no longer be read");

    /* The setting as a set */
    check(apps_hidden_parse(NULL) == 0 && apps_hidden_parse("") == 0 && apps_hidden_parse(" , ,") == 0, "nothing hides something");
    check(apps_hidden_parse("zello") == bit(APPID_ZELLO), "one name");
    check(apps_hidden_parse("zello,dmr,cb") == (bit(APPID_ZELLO) | bit(APPID_DMR) | bit(APPID_CB)), "three names");
    check(apps_hidden_parse("cb, zello  dmr\n") == (bit(APPID_ZELLO) | bit(APPID_DMR) | bit(APPID_CB)), "any order, spaces, a line end");
    check(apps_hidden_parse("dmr,dmr,dmr") == bit(APPID_DMR), "a name three times");
    check(apps_hidden_parse("zello,holodeck,dmr") == (bit(APPID_ZELLO) | bit(APPID_DMR)), "a name that is not known between two that are");
    check(apps_hidden_parse("holodeck") == 0, "only a name that is not known");
    check(apps_hidden_parse("Zello,DMR") == 0, "capitals are not the names");
    check(apps_hidden_parse("zell,zellos,dm") == 0, "a part of a name, a name with more");
    check(apps_hidden_parse("settings") == 0 && apps_hidden_parse("zello,settings") == bit(APPID_ZELLO), "Settings hidden by the setting");
    check(apps_hidden_parse("swrscan") == bit(APPID_SWRSCAN) && apps_hidden_parse("scanner") == bit(APPID_SCAN), "Scanner and SWR Scan apart");

    /* ... and back */
    apps_hidden_format(0, text, sizeof(text));
    check(!strcmp(text, ""), "nothing hidden is not an empty text");
    apps_hidden_format(bit(APPID_CB) | bit(APPID_ZELLO) | bit(APPID_DMR), text, sizeof(text));
    check(!strcmp(text, "zello,dmr,cb"), "the text is not in the apps' order with commas");
    apps_hidden_format(bit(APPID_ZELLO) | bit(APPID_SETTINGS), text, sizeof(text));
    check(!strcmp(text, "zello"), "Settings in the text");
    apps_hidden_format(all, text, sizeof(text));
    check(strlen(text) < APPS_HIDDEN_LEN - 1 && apps_hidden_parse(text) == all, "every app hidden does not fit the text, or reads back otherwise");
    for (int id = 0; id < APPID_COUNT; id++) {
        if (!apps_list_can_hide(id)) continue;
        apps_hidden_format(bit(id), text, sizeof(text));
        check(!strcmp(text, apps_list_key(id)) && apps_hidden_parse(text) == bit(id), "one app, there and back");
    }
    apps_hidden_format(bit(APPID_CLUSTER) | bit(APPID_PROPAGATION) | bit(APPID_CB), small, sizeof(small));
    check(!strcmp(small, "cluster"), "short room: not whole names only");
    small[0] = 'x';
    apps_hidden_format(all, small, 1);
    check(small[0] == '\0', "room for nothing is not an empty text");

    /* An app added later: the setting of before does not name it */
    hidden = apps_hidden_parse("zello,dmr");
    for (int id = 0; id < APPID_COUNT; id++) {
        if (id != APPID_ZELLO && id != APPID_DMR) check(!apps_hidden_has(hidden, id), "an app the setting does not name is hidden");
    }
    check(apps_hidden_has(hidden, APPID_ZELLO) && apps_hidden_has(hidden, APPID_DMR), "a named app is shown");
    check(!apps_hidden_has(~0u, APPID_SETTINGS) && !apps_hidden_has(~0u, APPID_COUNT) && !apps_hidden_has(~0u, -1),
          "Settings or an id that is none is hidden in a set of all bits");

    /* One app hidden and shown */
    hidden = apps_hidden_set(0, APPID_ZELLO, true);
    check(hidden == bit(APPID_ZELLO), "hiding one app");
    hidden = apps_hidden_set(hidden, APPID_CB, true);
    check(apps_hidden_set(hidden, APPID_ZELLO, false) == bit(APPID_CB), "showing one app changes another");
    check(apps_hidden_set(hidden, APPID_ZELLO, true) == hidden, "hiding a hidden app changes the set");
    check(apps_hidden_set(hidden, APPID_SETTINGS, true) == hidden, "Settings hidden by its switch");
    check(apps_hidden_set(hidden, APPID_COUNT, true) == hidden && apps_hidden_set(hidden, -1, true) == hidden, "an id that is none hidden");

    /* The pages: nothing hidden, the six of before */
    check(BAR_N == 23 && apps_shown(bar, BAR_N, 0, out) == 23 && !memcmp(out, bar, sizeof(bar)), "nothing hidden: not all 23 in order");
    check(apps_pages(23, APPS_PER_PAGE) == 6, "23 apps are not six pages");
    check(at(0, 0, 0) == APPID_MODES && at(0, 0, 3) == APPID_RECORDER, "APP 1:6");
    check(at(0, 1, 0) == APPID_SETTINGS && at(0, 1, 3) == APPID_CHANNELS, "APP 2:6");
    check(at(0, 2, 2) == APPID_SCREEN_REC && at(0, 3, 0) == APPID_PROPAGATION && at(0, 4, 3) == APPID_CB, "APP 3:6 to 5:6");
    check(at(0, 5, 0) == APPID_METEO && at(0, 5, 1) == APPID_MAIL && at(0, 5, 2) == APPID_DMR && at(0, 5, 3) == -1, "APP 6:6");

    /* Hidden: the others close up, in order */
    hidden = apps_hidden_parse("cluster");
    check(apps_shown(bar, BAR_N, hidden, out) == 22 && out[0] == APPID_MODES && out[1] == APPID_SWRSCAN, "Cluster hidden: 22, SWR Scan second");
    check(at(hidden, 0, 1) == APPID_SWRSCAN && at(hidden, 0, 3) == APPID_SETTINGS && at(hidden, 1, 0) == APPID_CWTRAINER,
          "Cluster hidden: page 1 does not close up, Settings fourth");
    check(apps_pages(22, APPS_PER_PAGE) == 6 && at(hidden, 5, 1) == APPID_DMR && at(hidden, 5, 2) == -1, "22 apps: the last page has two");
    hidden = apps_hidden_parse("swrscan,cwtrainer,screenrec,walkie,birdie,iradio,zello,svx,dmr,flights,aero,cb");
    check(apps_shown(bar, BAR_N, hidden, out) == 13 && apps_pages(13, APPS_PER_PAGE) == 4, "12 hidden, 10 of them on the bar: not 13 on four pages");
    check(at(hidden, 0, 0) == APPID_MODES && at(hidden, 0, 1) == APPID_CLUSTER && at(hidden, 0, 2) == APPID_RECORDER &&
          at(hidden, 0, 3) == APPID_SETTINGS, "13 left: page 1");
    check(at(hidden, 2, 3) == APPID_METEO && at(hidden, 3, 0) == APPID_MAIL && at(hidden, 3, 1) == -1, "13 left: pages 3 and 4");
    /* no gap: on every page in use every place before the first empty one has an app, and only the last has empty ones */
    for (uint32_t h = 0; h < 4096; h++) {
        uint32_t set = (h * 2654435761u) & all;     /* 4096 sets spread over the apps */
        int      n = apps_shown(bar, BAR_N, set, out), pages = apps_pages(n, APPS_PER_PAGE), k = 0;
        bool     ok = pages >= 1 && pages <= 6 && n >= 1;

        for (int p = 0; p < pages; p++) {
            for (int s = 0; s < APPS_PER_PAGE; s++) {
                int id = at(set, p, s);

                ok &= k < n ? id == out[k] : id == -1;
                ok &= id == -1 || !apps_hidden_has(set, id);
                k++;
            }
        }
        ok &= k >= n && k - n < APPS_PER_PAGE;      /* the last page is not empty and not past the end */
        for (int s = 0; s < APPS_PER_PAGE; s++) ok &= at(set, pages, s) == at(set, pages - 1, s) && at(set, 5, s) == at(set, pages - 1, s);
        check(ok, "a set of hidden apps leaves a gap, a hidden app or an empty page");
    }
    /* all but Settings */
    check(apps_shown(bar, BAR_N, all, out) == 1 && out[0] == APPID_SETTINGS, "everything hidden: Settings is not left");
    check(apps_pages(1, APPS_PER_PAGE) == 1 && at(all, 0, 0) == APPID_SETTINGS && at(all, 0, 1) == -1, "everything hidden: not one page with Settings");
    check(at(all, 3, 0) == APPID_SETTINGS && at(all, 5, 0) == APPID_SETTINGS, "a page that is gone does not have the last one's apps");
    check(apps_pages(0, APPS_PER_PAGE) == 1 && apps_pages(4, APPS_PER_PAGE) == 1 && apps_pages(5, APPS_PER_PAGE) == 2, "the pages of 0, 4, 5 apps");
    check(at(0, -1, 0) == -1 && at(0, 0, -1) == -1 && at(0, 0, APPS_PER_PAGE) == -1, "a place that is none has an app");

    if (argc > 1) sources(argv[1]);

    printf("%d cases, %d wrong\n", cases, bad);
    return bad ? 1 : 0;
}
