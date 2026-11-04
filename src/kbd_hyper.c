/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - CapsLock as a hyper key on a USB or Bluetooth keyboard (see kbd_hyper.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "kbd_hyper.h"

#include <stddef.h>

#define LONG_BITS (8 * sizeof(long))

/* CapsLock + a key, by scancode: where it is on a US keyboard, as the radio types (the letters here are in the same
 * place on AZERTY and QWERTZ ones; on AZERTY 1 is the key marked & and 1). A chord is added here, a line;
 * main_screen_chord (screen_main.c) runs its action and chooses where it goes (L in CW with a line typed, 1-8 in
 * JTTY) */
static const struct {
    uint16_t           code;
    kbd_hyper_action_t action;
} chords[] = {
    {KEY_L, HYPER_LOG_QSO},      {KEY_K, HYPER_LOCK},         {KEY_P, HYPER_SCREENSHOT},   {KEY_S, HYPER_SETTINGS},
    {KEY_F, HYPER_FREQ},         {KEY_UP, HYPER_BAND_UP},     {KEY_DOWN, HYPER_BAND_DOWN}, {KEY_SPACE, HYPER_PTT},
    {KEY_1, HYPER_KEY_1},        {KEY_2, HYPER_KEY_2},        {KEY_3, HYPER_KEY_3},        {KEY_4, HYPER_KEY_4},
    {KEY_5, HYPER_KEY_5},        {KEY_6, HYPER_KEY_6},        {KEY_7, HYPER_KEY_7},        {KEY_8, HYPER_KEY_8},
};

/* Held with CapsLock, these still reach xkb: Shift let go during a chord would otherwise stay down there */
static bool modifier(uint16_t code) {
    switch (code) {
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
        case KEY_LEFTALT: case KEY_RIGHTALT: case KEY_LEFTMETA: case KEY_RIGHTMETA:
            return true;
        default:
            return false;
    }
}

kbd_hyper_action_t kbd_hyper_action(uint16_t code) {
    for (size_t i = 0; i < sizeof(chords) / sizeof(chords[0]); i++) {
        if (chords[i].code == code) {
            return chords[i].action;
        }
    }
    return HYPER_NONE;
}

kbd_hyper_use_t kbd_hyper_key(kbd_hyper_t *h, uint16_t code, int state, kbd_hyper_action_t *action) {
    if (code >= KEY_CNT) {
        return HYPER_PASS;
    }
    unsigned long *word = &h->taken[code / LONG_BITS], bit = 1UL << (code % LONG_BITS);

    if (code == KEY_CAPSLOCK) {
        if (state == 1) {
            h->held  = true;
            h->chord = false;
        } else if (state == 0 && h->held) {
            h->held = false;
            return h->chord ? HYPER_DROP : HYPER_CAPS;
        }
        return HYPER_DROP;
    }
    /* A chord's key: its repeats and its release too, also once CapsLock is let go */
    if (*word & bit) {
        if (state == 0) {
            *word &= ~bit;
        }
        return HYPER_DROP;
    }
    /* A key down before CapsLock went down repeats and goes up as usual */
    if (!h->held || state != 1 || modifier(code)) {
        return HYPER_PASS;
    }
    *word   |= bit;
    h->chord = true;
    *action  = kbd_hyper_action(code);
    return HYPER_CHORD;
}
