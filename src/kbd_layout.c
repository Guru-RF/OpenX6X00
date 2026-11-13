/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The keyboard's layouts (see kbd_layout.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "kbd_layout.h"

#include <linux/input.h>
#include <stddef.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

/* The first is the one of v0.3.9 and before (lv_drv_conf.h XKB_KEY_MAP). Saved as the index: add at the end only */
static const kbd_layout_t layouts[KBD_LAYOUTS] = {
    {"US",                  "us", "pc101", false},
    {"UK",                  "gb", "pc105", false},
    {"German (QWERTZ)",     "de", "pc105", false},
    {"Swiss German",        "ch", "pc105", false},
    {"French (AZERTY)",     "fr", "pc105", true},     /* xkb's own list of the layouts its option is for */
    {"Belgian (AZERTY)",    "be", "pc105", true},
    {"Spanish",             "es", "pc105", false},
    {"Italian",             "it", "pc105", false},
    {"Portuguese",          "pt", "pc105", false},
    {"Swedish",             "se", "pc105", false},
    {"Finnish",             "fi", "pc105", false},
    {"Danish",              "dk", "pc105", false},
    {"Norwegian",           "no", "pc105", false},
    {"Polish",              "pl", "pc105", false},
    {"Czech (QWERTZ)",      "cz", "pc105", false},
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

uint32_t kbd_layout_digits_row(struct xkb_state *st, int code) {
    const xkb_keysym_t *syms;
    xkb_keycode_t       kc = code + 8;      /* evdev to xkb, as xkb.c */
    xkb_layout_index_t  group;
    xkb_level_index_t   level;
    char                buf[8];
    uint32_t            out = 0;

    if (!st || code < KEY_1 || code > KEY_0) {
        return 0;
    }
    group = xkb_state_key_get_layout(st, kc);
    level = xkb_state_key_get_level(st, kc, group);
    /* Level 0 is the key alone, 1 with Shift; 2 and 3 are AltGr's, which Caps Lock leaves as they are */
    if (level > 1 || xkb_keymap_key_get_syms_by_level(xkb_state_get_keymap(st), kc, group, !level, &syms) != 1) {
        return 0;
    }
    if (xkb_keysym_to_utf8(syms[0], buf, sizeof(buf)) <= 1 || strlen(buf) > sizeof(out)) {
        return 0;
    }
    memcpy(&out, buf, strlen(buf));
    return out;
}
