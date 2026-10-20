/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* A popup with one line of text to type (textarea_window.c) */

#ifndef X6100_TEXTAREA_WINDOW_H
#define X6100_TEXTAREA_WINDOW_H

#include "lvgl/lvgl.h"
#include "gui_events.h"
#include <stdint.h>
#include <unistd.h>

/* Keys that type nothing: the radio's knobs and keys and the hand mic's (events.h), the USB
 * keyboard's F-keys, Page Up/Down and the like (keyboard.h). LVGL's text box would insert
 * their codes as characters that show as nothing */
static inline bool textarea_key_writes_nothing(uint32_t key) {
    bool radio = key >= KEY_VOL_LEFT_EDIT && key <= HKEY_UNKNOWN;
    bool pc    = key >= 0xFD00 && key <= 0xFFFF;

    return radio || pc;
}

/* OK: true closes the window, false keeps it open (the text is not accepted) */
typedef bool (*textarea_window_cb_t)(void);

lv_obj_t *textarea_window_open(textarea_window_cb_t ok_cb, textarea_window_cb_t cancel_cb);
/* The same with a title in front of the text */
lv_obj_t *textarea_window_open_w_label(textarea_window_cb_t ok_cb, textarea_window_cb_t cancel_cb, const char *text);
void textarea_window_close();

const char *textarea_window_get();
void textarea_window_set(const char *text);
lv_obj_t *textarea_window_text();

#endif
