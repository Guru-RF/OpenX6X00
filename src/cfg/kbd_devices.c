/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - What each keyboard's [ ] keys are, device by device (see kbd_devices.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "kbd_devices.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

void kbd_dev_id(char *out, unsigned bus, unsigned vendor, unsigned product) {
    snprintf(out, KBD_DEV_ID_LEN, "%04x:%04x:%04x", bus & 0xffff, vendor & 0xffff, product & 0xffff);
}

/* The word at w (up to a space or the end) is an id: four hex digits, a colon, four, a colon, four */
static bool well_formed(const char *w, size_t n) {
    if (n != KBD_DEV_ID_LEN - 1) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((i == 4 || i == 9) ? w[i] != ':' : !isxdigit((unsigned char)w[i])) {
            return false;
        }
    }
    return true;
}

bool kbd_dev_listed(const char *list, const char *id) {
    size_t n = strlen(id);

    for (const char *w = list; w && *w;) {
        size_t len = strcspn(w, " ");

        if (len == n && n && strncasecmp(w, id, n) == 0) {
            return true;
        }
        w += len;
        w += strspn(w, " ");
    }
    return false;
}

/* The ids of list, well-formed and in lower case, each once in its last place; and id last when on */
static void rebuild(const char *list, const char *id, bool on, char *out, size_t size) {
    char ids[KBD_DEV_MAX * 4 + 1][KBD_DEV_ID_LEN];
    int  count = 0;

    for (const char *w = list ? list : ""; *w;) {
        size_t len = strcspn(w, " ");

        if (well_formed(w, len) && count < (int)(sizeof(ids) / sizeof(ids[0])) - 1) {
            for (size_t i = 0; i < len; i++) {
                ids[count][i] = (char)tolower((unsigned char)w[i]);
            }
            ids[count++][len] = '\0';
        }
        w += len;
        w += strspn(w, " ");
    }
    if (id && on && well_formed(id, strlen(id))) {
        for (size_t i = 0; i <= strlen(id); i++) {
            ids[count][i] = (char)tolower((unsigned char)id[i]);
        }
        count++;
    }
    /* From the end: a later copy wins, the oldest past KBD_DEV_MAX drop out */
    bool keep[sizeof(ids) / sizeof(ids[0])] = {false};
    int  kept = 0;

    for (int i = count - 1; i >= 0 && kept < KBD_DEV_MAX; i--) {
        bool dup = id && !on && strcasecmp(ids[i], id) == 0;

        for (int j = i + 1; j < count && !dup; j++) {
            dup = keep[j] && strcmp(ids[i], ids[j]) == 0;
        }
        if (!dup) {
            keep[i] = true;
            kept++;
        }
    }
    size_t o = 0;

    if (size) {
        out[0] = '\0';
    }
    for (int i = 0; i < count; i++) {
        if (keep[i] && o + strlen(ids[i]) + (o ? 1 : 0) < size) {
            o += snprintf(out + o, size - o, "%s%s", o ? " " : "", ids[i]);
        }
    }
}

void kbd_dev_list_clean(const char *list, char *out, size_t size) {
    rebuild(list, NULL, false, out, size);
}

void kbd_dev_list_set(const char *list, const char *id, bool on, char *out, size_t size) {
    rebuild(list, id, on, out, size);
}
