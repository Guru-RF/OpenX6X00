/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the apps of the APP menu, and the ones the owner hid (see apps_list.h, #351)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "apps_list.h"

#include <stdio.h>
#include <string.h>

/* key: in the setting, never changed and never used for another app. name: as the APP grid's tile has it */
static const struct {
    const char *key, *name;
} apps[APPID_COUNT] = {
    [APPID_MODES]       = {"modes", "Modes"},
    [APPID_CLUSTER]     = {"cluster", "Cluster"},
    [APPID_FIELD]       = {"field", "Field Ops"},
    [APPID_BANDS]       = {"bands", "Bands"},
    [APPID_PROPAGATION] = {"propagation", "Propagation"},
    [APPID_CHANNELS]    = {"channels", "Channels"},
    [APPID_SCAN]        = {"scanner", "Scanner"},
    [APPID_SWRSCAN]     = {"swrscan", "SWR Scan"},
    [APPID_RECORDER]    = {"recorder", "Recorder"},
    [APPID_CWTRAINER]   = {"cwtrainer", "CW Trainer"},
    [APPID_CW_KBD]      = {"cwkbd", "CW Keyboard"},
    [APPID_MAIL]        = {"mail", "Mail"},
    [APPID_GPS]         = {"gps", "GPS / Clock"},
    [APPID_SCREEN_REC]  = {"screenrec", "Screen Rec"},
    [APPID_WALKIE]      = {"walkie", "Walkie"},
    [APPID_BIRDIE]      = {"birdie", "Birdie Cal"},
    [APPID_IRADIO]      = {"iradio", "Web radio"},
    [APPID_ZELLO]       = {"zello", "Zello"},
    [APPID_SVX]         = {"svx", "SVXConnect"},
    [APPID_DMR]         = {"dmr", "DMR"},
    [APPID_FLIGHTS]     = {"flights", "Flights"},
    [APPID_AERO]        = {"aero", "HF Air Band"},
    [APPID_CB]          = {"cb", "CB"},
    [APPID_METEO]       = {"weather", "Weather"},
    [APPID_SETTINGS]    = {"settings", "Settings"},
};

_Static_assert(APPID_COUNT <= 32, "the hidden set is 32 bits");

static bool known(int id) {
    return id >= 0 && id < APPID_COUNT;
}

const char *apps_list_key(int id) {
    return known(id) ? apps[id].key : NULL;
}

const char *apps_list_name(int id) {
    return known(id) ? apps[id].name : NULL;
}

bool apps_list_can_hide(int id) {
    return known(id) && id != APPID_SETTINGS;
}

int apps_list_find(const char *key) {
    for (int id = 0; key && id < APPID_COUNT; id++) {
        if (!strcmp(apps[id].key, key)) return id;
    }
    return -1;
}

bool apps_hidden_has(uint32_t hidden, int id) {
    return apps_list_can_hide(id) && (hidden >> id & 1u);
}

uint32_t apps_hidden_set(uint32_t hidden, int id, bool hide) {
    if (!apps_list_can_hide(id)) return hidden;
    return hide ? hidden | 1u << id : hidden & ~(1u << id);
}

uint32_t apps_hidden_parse(const char *text) {
    uint32_t hidden = 0;

    while (text && *text) {
        size_t len = strcspn(text, ", \t\r\n");

        for (int id = 0; len && id < APPID_COUNT; id++) {
            if (strlen(apps[id].key) == len && !strncmp(apps[id].key, text, len)) {
                hidden = apps_hidden_set(hidden, id, true);
                break;
            }
        }
        text += len;
        text += strspn(text, ", \t\r\n");
    }
    return hidden;
}

void apps_hidden_format(uint32_t hidden, char *out, size_t size) {
    size_t l = 0;

    if (!size) return;
    out[0] = '\0';
    for (int id = 0; id < APPID_COUNT; id++) {
        if (!apps_hidden_has(hidden, id)) continue;
        if (l + strlen(apps[id].key) + 2 > size) break;     /* a name whole, or not at all */
        l += (size_t)snprintf(out + l, size - l, "%s%s", l ? "," : "", apps[id].key);
    }
}

int apps_shown(const uint8_t *order, int n, uint32_t hidden, uint8_t *out) {
    int shown = 0;

    for (int i = 0; i < n; i++) {
        if (!apps_hidden_has(hidden, order[i])) out[shown++] = order[i];
    }
    return shown;
}

int apps_pages(int shown, int per) {
    if (shown <= 0 || per <= 0) return 1;
    return (shown + per - 1) / per;
}

int apps_page_app(const uint8_t *order, int n, uint32_t hidden, int per, int page, int slot) {
    int shown = 0, pages, want;

    if (page < 0 || slot < 0 || slot >= per) return -1;
    for (int i = 0; i < n; i++) shown += !apps_hidden_has(hidden, order[i]);
    pages = apps_pages(shown, per);
    want  = (page < pages ? page : pages - 1) * per + slot;
    for (int i = 0; i < n; i++) {
        if (apps_hidden_has(hidden, order[i])) continue;
        if (want-- == 0) return order[i];
    }
    return -1;
}
