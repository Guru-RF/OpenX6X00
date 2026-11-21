/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - sending in the RTTY app (#312), see rtty_send.c
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "button_bar.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The RTTY pages after the first (button_bar.cpp's Rate, Shift, Center and Reverse): the first and the last of
 * them, for its page button's press and hold */
extern buttons_page_t rtty_page_send;
extern buttons_page_t rtty_page_power;

/* RTTY opens: the macros read again (the web configurator may have changed them), the buttons' labels with them.
 * LVGL thread */
void rtty_send_open(void);

/* A main screen key: with a keyboard in RTTY, a letter or digit opens the line to send with it typed; true then */
bool rtty_send_key(uint32_t key);

/* CapsLock+1 to 6 (screen_main.c): RTTY's macro n sent, as its button. False: RTTY is not open, or no such macro */
bool rtty_send_macro(int n);

#ifdef __cplusplus
}
#endif
