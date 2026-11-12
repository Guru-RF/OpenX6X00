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

/* A keyboard is plugged in and open */
bool keyboard_ready();

/* What CapsLock and a key held together do, a kbd_hyper_action_t (kbd_hyper.h): run once the key's read is over */
void keyboard_chord(int action);
