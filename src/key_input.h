/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "kbd_keymap.h"    /* KEYBOARD_SCRL_LOCK, KEYBOARD_PRINT */
#include "lvgl/lvgl.h"

/* The group of the knobs, the buttons and the keyboard: the focused widget gets their keys */
extern lv_group_t *keyboard_group;

void keyboard_init();

/* The keyboard's layout follows Settings > Keys > KEYBOARD from now on: after the settings are read (#236) */
void keyboard_layout_start(void);

/* A USB or Bluetooth device with keys of a keyboard's (letters, [ ] or the volume keys), as Settings > Keys >
 * KEYBOARD lists it to choose what its [ ] are (kbd_devices.h, #306) */
typedef struct {
    char name[40];      /* the kernel's, from its node with the letters */
    char id[16];        /* kbd_dev_id(): bus, vendor and product */
    bool bluetooth;
} keyboard_dev_t;

/* Those plugged in or connected now, each once (a keyboard's several nodes are one device), at most n into out */
size_t keyboard_devices(keyboard_dev_t *out, size_t n);

/* A keyboard is plugged in and open */
bool keyboard_ready();

/* What CapsLock and a key held together do, a kbd_hyper_action_t (kbd_hyper.h): run once the key's read is over */
void keyboard_chord(int action);
