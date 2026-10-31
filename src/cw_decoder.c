/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW decoder output.
 */

#include "cw_decoder.h"

#include "lcd_panel.h"
#include "panel.h"
#include "remote_server.h"

#include <stdio.h>
#include <string.h>

static void (*to_text)(const char *);
static void (*to_wpm)(int);

void cw_decoder_set_sink(void (*text)(const char *), void (*wpm)(int)) {
    to_text = text;
    to_wpm  = wpm;
}

void cw_decoder_text(const char *text) {
    remote_server_cw_text(text);    /* the browser panel too, wherever the text goes here */
    if (to_text) {
        to_text(text);
        return;
    }
    lcd_panel_cw_text(text);        /* KX3Lovers Plus shows it in its spectrum strip */
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
