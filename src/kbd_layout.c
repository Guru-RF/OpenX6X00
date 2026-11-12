/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The keyboard's layouts (see kbd_layout.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "kbd_layout.h"

#include <stddef.h>

/* The first is the one of v0.3.9 and before (lv_drv_conf.h XKB_KEY_MAP). Saved as the index: add at the end only */
static const kbd_layout_t layouts[KBD_LAYOUTS] = {
    {"US",                  "us", "pc101"},
    {"UK",                  "gb", "pc105"},
    {"German (QWERTZ)",     "de", "pc105"},
    {"Swiss German",        "ch", "pc105"},
    {"French (AZERTY)",     "fr", "pc105"},
    {"Belgian (AZERTY)",    "be", "pc105"},
    {"Spanish",             "es", "pc105"},
    {"Italian",             "it", "pc105"},
    {"Portuguese",          "pt", "pc105"},
    {"Swedish",             "se", "pc105"},
    {"Finnish",             "fi", "pc105"},
    {"Danish",              "dk", "pc105"},
    {"Norwegian",           "no", "pc105"},
    {"Polish",              "pl", "pc105"},
    {"Czech (QWERTZ)",      "cz", "pc105"},
};

const kbd_layout_t *kbd_layout_get(int i) {
    return &layouts[i >= 0 && i < KBD_LAYOUTS ? i : 0];
}

const char *kbd_layout_options(void) {
    static char opts[KBD_LAYOUTS * 24];
    char       *p = opts;

    if (!opts[0]) {
        for (int i = 0; i < KBD_LAYOUTS; i++) {
            *p++ = ' ';     /* " US \n UK \n...", as the other choices in Settings */
            for (const char *s = layouts[i].label; *s; s++)
                *p++ = *s;
            *p++ = ' ';
            if (i < KBD_LAYOUTS - 1)
                *p++ = '\n';
        }
        *p = '\0';
    }
    return opts;
}
