/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW decoder output.
 */

#include "cw_decoder.h"

#include "panel.h"

#include <stdio.h>
#include <string.h>

static void (*to_text)(const char *);
static void (*to_wpm)(int);

void cw_decoder_set_sink(void (*text)(const char *), void (*wpm)(int)) {
    to_text = text;
    to_wpm  = wpm;
}

void cw_decoder_text(const char *text) {
    if (to_text) {
        to_text(text);
        return;
    }
    panel_add_text(text);
}

void cw_decoder_wpm(int wpm) {
    char info[16];

    if (to_wpm) {
        to_wpm(wpm);
        return;
    }
    snprintf(info, sizeof(info), "WPM: %d", wpm);
    panel_set_info(info);
}
