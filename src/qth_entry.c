/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - QTH locator editor (APP action "QTH grid")
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#include "qth_entry.h"

#include "gui_events.h"
#include "radio.h"
#include "station.h"
#include "text_popup.h"

static void open_cb(lv_obj_t *parent);
static void close_cb(void);
static void text_key_cb(lv_event_t *e);

static dialog_t dialog = { .construct_cb = open_cb, .destruct_cb = close_cb, .key_cb = text_key_cb };

dialog_t *dialog_qth = &dialog;

static bool just_close() {
    dialog_destruct();
    return true;     /* the editor goes with the dialog */
}

/* A wrong grid is refused with a message by station.c; the editor closes either way */
static bool save_and_close() {
    station_qth_save(textarea_window_get());
    return just_close();
}

/* With a keyboard on USB: ENTER saves, ESC leaves, VOL keeps the volume */
static void text_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ENTER)
        save_and_close();
    else if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}

static void open_cb(lv_obj_t *parent) {
    lv_obj_t *text;

    dialog.obj = textarea_window_open(save_and_close, just_close);
    text = textarea_window_text();
    station_qth_edit(text);         /* grid characters only, the saved one to start from */
    lv_obj_add_event_cb(text, text_key_cb, LV_EVENT_KEY, NULL);
}

static void close_cb(void) {
    textarea_window_close();
    dialog.obj = NULL;
}
