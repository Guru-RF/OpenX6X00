/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW decoder output.
 */

#include "cw_decoder.h"

#include "freq_strip.h"
#include "lcd_panel.h"
#include "panel.h"
#include "remote_server.h"
#include "scheduler.h"
#include "voice_text.h"

#include <stdio.h>
#include <string.h>

static void (*to_text)(const char *, bool);
static void (*to_wpm)(int);

void cw_decoder_set_sink(void (*text)(const char *, bool), void (*wpm)(int)) {
    to_text = text;
    to_wpm  = wpm;
}

void cw_decoder_text(const char *text, bool own) {
    remote_server_cw_text(text);    /* the browser panel too, wherever the text goes here */
    if (!own) {
        voice_text_add(text);       /* read aloud, the main screen's and the CW apps' alike; not what we send */
    }
    if (to_text) {
        to_text(text, own);
        return;
    }
    lcd_panel_cw_text(text);        /* Amber Trey shows it in its spectrum strip */
    panel_add_text(text);
}

/* LVGL thread: the speed in the frequency strip too, where Settings may put it (#308) */
static void strip_wpm(void *arg) {
    freq_strip_set_wpm(*(int *)arg);
}

void cw_decoder_wpm(int wpm) {
    char info[16];

    if (to_wpm) {
        to_wpm(wpm);
        return;
    }
    snprintf(info, sizeof(info), "WPM: %d", wpm);
    panel_set_info(info);
    scheduler_put(strip_wpm, &wpm, sizeof(wpm));
}
