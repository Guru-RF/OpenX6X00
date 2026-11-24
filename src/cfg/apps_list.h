/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the apps of the APP menu, and the ones the owner hid (apps_list.c, #351)
 *
 *  The APP grid (apps_menu.c) and the APP button pages (button_bar.cpp) grow with every app. Settings > Apps, and
 *  the web Display page, take an app out of both; this file has the apps, each with a name for the setting that
 *  never changes, and the rule.
 *
 *  The setting (apps_hidden) holds the names of the hidden apps, "zello,dmr,cb": what is hidden, not what is shown,
 *  so an app a later version adds is shown without anything done, and a name a version does not know (an app a
 *  later one had) is left out as it is read. Settings is never hidden: the page that brings the others back is in
 *  it. An empty setting shows everything, as before.
 *
 *  Hidden only takes an app out of the menus. A key given to it in Settings > Keys, a keyboard's key and anything
 *  else that opens it goes on as before, and so does an app that is running or has something under way.
 *
 *  No LVGL here: tools/apps_hide tests it.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In the order of the APP grid's tiles. A new app: a line here and in apps_list.c, never a name used again */
typedef enum {
    APPID_MODES = 0,
    APPID_CLUSTER,
    APPID_FIELD,
    APPID_BANDS,
    APPID_PROPAGATION,
    APPID_CHANNELS,
    APPID_SCAN,
    APPID_SWRSCAN,
    APPID_RECORDER,
    APPID_CWTRAINER,
    APPID_CW_KBD,
    APPID_MAIL,
    APPID_GPS,
    APPID_SCREEN_REC,
    APPID_WALKIE,
    APPID_BIRDIE,
    APPID_IRADIO,
    APPID_ZELLO,
    APPID_SVX,
    APPID_DMR,
    APPID_FLIGHTS,
    APPID_AERO,
    APPID_CB,
    APPID_METEO,
    APPID_SETTINGS,     /* never hidden */
    APPID_COUNT,
} app_id_t;

#define APPS_PER_PAGE   4       /* apps on an APP button page, after its page button */
#define APPS_HIDDEN_LEN 256     /* the setting's text at its longest, with room for more apps */

/* The app's name in the setting ("zello"), its name on a tile and in Settings ("Zello"); NULL for an id that is
 * none */
const char *apps_list_key(int id);
const char *apps_list_name(int id);

/* True for an app that can be hidden: every one but Settings */
bool apps_list_can_hide(int id);

/* The app of a setting name; -1 when there is none */
int apps_list_find(const char *key);

/* The setting's text as a set, bit id for a hidden app. Names apart by commas or spaces; one that is not known and
 * Settings' are left out; NULL is none */
uint32_t apps_hidden_parse(const char *text);

/* The set as the setting keeps it: the names in the apps' order with commas between, "" for none */
void apps_hidden_format(uint32_t hidden, char *out, size_t size);

/* The set with app id hidden or shown; as it was for Settings and for an id that is none */
uint32_t apps_hidden_set(uint32_t hidden, int id, bool hide);

bool apps_hidden_has(uint32_t hidden, int id);

/* A menu's apps (order, n of them, in the menu's own order) less the hidden ones, closed up, into out (room for
 * n); how many are left */
int apps_shown(const uint8_t *order, int n, uint32_t hidden, uint8_t *out);

/* The pages `shown` apps take, `per` to a page; one page for none, an empty one */
int apps_pages(int shown, int per);

/* The app at place `slot` (0 to per - 1) of page `page` (from 0) when the menu's shown apps fill pages of `per` in
 * the menu's order; -1 for a place left empty on the last page. A page past the last one in use has the last
 * one's apps: a window that comes back to a page that is no longer there finds apps on it, not an empty bar */
int apps_page_app(const uint8_t *order, int n, uint32_t hidden, int per, int page, int slot);

#ifdef __cplusplus
}
#endif
