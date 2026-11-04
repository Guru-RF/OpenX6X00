/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - CapsLock as a hyper key on a USB or Bluetooth keyboard (kbd_hyper.c)
 *
 *  While CapsLock is held, a key pressed with it is a chord: CapsLock+Q logs a QSO, and the others in
 *  kbd_hyper.c's table. A chord's keys are never typed, and CapsLock held for a chord leaves Caps
 *  Lock as it was. CapsLock tapped alone, no key pressed while it was down, is Caps Lock as before; it turns
 *  when the tap ends. Shift, Ctrl, Alt and the Meta keys go through as usual while it is held. Here only what
 *  each key event becomes, from the scancodes: key_input.c does it.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <linux/input.h>    /* KEY_CNT */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a chord does (screen_main.c's main_screen_chord runs it); the table of chords is in kbd_hyper.c */
typedef enum {
    HYPER_NONE = 0,     /* a chord with no action: nothing at all */
    HYPER_LOG_QSO,      /* Q: in CW the CW line's QSO logged, else the main screen's quick QSO log */
    HYPER_LOCK,         /* L: the dial lock on or off, as Scroll Lock */
    HYPER_SCREENSHOT,   /* P: a screenshot, as Print Screen */
    HYPER_SETTINGS,     /* S: Settings */
    HYPER_FREQ,         /* F: the frequency box, as f (also in CW, where f goes into the CW line) */
    HYPER_BAND_UP,      /* Up: the next band up, as the BAND keys */
    HYPER_BAND_DOWN,    /* Down: the next band down */
    HYPER_KEY_1,        /* 1 to 8, in order: CW messages 1-5 on the main screen, JTTY's macros 1-8 */
    HYPER_KEY_2,
    HYPER_KEY_3,
    HYPER_KEY_4,
    HYPER_KEY_5,
    HYPER_KEY_6,
    HYPER_KEY_7,
    HYPER_KEY_8,
} kbd_hyper_action_t;

/* What becomes of one key event */
typedef enum {
    HYPER_PASS,         /* an ordinary key: on to xkb and LVGL */
    HYPER_DROP,         /* CapsLock's own, or a chord key's repeat or release: nobody sees it */
    HYPER_CAPS,         /* CapsLock let go after a tap alone: Caps Lock turns (the event is dropped too) */
    HYPER_CHORD,        /* a key down while CapsLock is held: its action, the key dropped */
} kbd_hyper_use_t;

typedef struct kbd_hyper {
    bool          held;     /* CapsLock down */
    bool          chord;    /* a key went down while it was: no Caps Lock at its release */
    unsigned long taken[(KEY_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))];   /* chord keys still down */
} kbd_hyper_t;

/* One evdev key event (state 0 up, 1 down, 2 auto-repeat). HYPER_CHORD gives the chord's action in *action */
kbd_hyper_use_t kbd_hyper_key(kbd_hyper_t *h, uint16_t code, int state, kbd_hyper_action_t *action);

/* The action of CapsLock + the key with this scancode, HYPER_NONE when it has none */
kbd_hyper_action_t kbd_hyper_action(uint16_t code);

#ifdef __cplusplus
}
#endif
