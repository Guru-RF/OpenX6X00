/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#include "app_window.h"
#include "board.h"
#include "theme.h"
#include "gui_styles.h"
#include "radio.h"
#include "freq_strip.h"
#include "cfg/cfg_api.h"
#include "key_input.h"
#include "gui_events.h"
#include "waterfall_view.h"
#include "knob_info.h"
#include "screen_main.h"

#include <stdio.h>
#include <string.h>

#define APP_FREQS BOARD_DATA "/app_freqs"   /* "<app> <freq> <mode>" a line */

static dialog_t *shown;              /* the window on screen (or the last one asked for) */
static int32_t   mode_back = -1;     /* the mode an app replaced, given back when it closes */
static int32_t   mode_forced;
static bool      radio_back;         /* dialog_restore_radio_on_close() */
static char      app_name[8];        /* dialog_app_memory(): the app being opened, "" none */

bool app_mem_load(const char *name, int32_t *freq, int32_t *mode) {
    FILE *f = fopen(APP_FREQS, "r");
    char  app[16];
    int   hz, md;

    if (!f) {
        return false;
    }
    while (fscanf(f, "%15s %d %d", app, &hz, &md) == 3) {
        if (!strcmp(app, name)) {
            fclose(f);
            *freq = hz;
            *mode = md;
            return true;
        }
    }
    fclose(f);
    return false;
}

/* The other apps' lines are kept (at most 16), this one's goes last */
void app_mem_save(const char *name) {
    char  keep[16][48], app[16];
    int   n = 0, hz, md;
    FILE *f = fopen(APP_FREQS, "r");

    if (f) {
        while (n < 16 && fscanf(f, "%15s %d %d", app, &hz, &md) == 3) {
            if (strcmp(app, name)) {
                snprintf(keep[n++], sizeof(keep[0]), "%s %d %d", app, hz, md);
            }
        }
        fclose(f);
    }
    f = fopen(APP_FREQS ".tmp", "w");
    if (!f) {
        return;
    }
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s\n", keep[i]);
    }
    fprintf(f, "%s %d %d\n", name, (int)cparam_i_get(cfg_fg_freq), (int)cparam_i_get(cfg_cur_mode));
    fclose(f);
    rename(APP_FREQS ".tmp", APP_FREQS);
}

void dialog_app_memory(const char *name) {
    snprintf(app_name, sizeof(app_name), "%s", name);
}

void dialog_restore_mode_on_close(int32_t mode, int32_t forced) {
    mode_back   = mode;
    mode_forced = forced;
}

void dialog_restore_radio_on_close(void) {
    if (dialog_is_run() && !shown->restore_radio) {
        mem_save(MEM_BACKUP_ID);
        radio_back = true;
    }
}

static void open_window(dialog_t *d, lv_obj_t *parent) {
    int32_t freq, mode;

    knobs_display(false);
    waterfall_refresh_period_set(2);    /* the waterfall slower under a window */
    main_screen_keys_enable(false);
    if (d->restore_radio) {
        mem_save(MEM_BACKUP_ID);
    }
    if (app_name[0] && app_mem_load(app_name, &freq, &mode)) {
        main_screen_set_freq(freq);
        cparam_i_set(cfg_cur_mode, mode);
    }
    d->prev_page = buttons_get_cur_page();
    buttons_unload_page();
    if (d->btn_page) {
        buttons_load_page(d->btn_page);
    }
    d->construct_cb(parent);
    d->run = true;
    freq_strip_set_dialog(true);
}

void dialog_construct(dialog_t *dialog, lv_obj_t *parent) {
    if (dialog && !dialog->run) {
        open_window(dialog, parent);
    }
    shown = dialog;
}

static void close_window(dialog_t *d) {
    lv_indev_t *indev = lv_indev_get_act();

    knobs_display(true);
    waterfall_refresh_reset();
    d->run = false;
    freq_strip_set_dialog(false);

    /* Closed by a key (ESC): after the key LVGL sends CANCEL to the focused object. destruct_cb
     * usually takes that object out of the group first, so LVGL cannot see it go and the CANCEL
     * hit freed memory (closing Modes with VOL crashed) */
    if (indev && d->obj) {
        lv_indev_reset(indev, d->obj);
    }
    if (d->destruct_cb) {
        d->destruct_cb();
    }
    if (app_name[0]) {
        app_mem_save(app_name);     /* the app's last frequency, before the radio goes back */
        app_name[0] = 0;
        radio_set_digi_cap(false);
    }
    if (d->obj) {
        lv_obj_del(d->obj);
        event_obj_drop_deleted();   /* what a thread queued for it meanwhile (the GPS window gets fixes) */
    }
    if (d->restore_radio || radio_back) {
        mem_load(MEM_BACKUP_ID);
        if (radio_back) {
            mode_back = -1;         /* the memory brought the mode back already */
        }
        radio_back = false;
    }
    buttons_unload_page();
    if (d->prev_page) {
        buttons_load_page(d->prev_page);
    }
    main_screen_keys_enable(true);
}

void dialog_destruct() {
    freq_strip_set_app(NULL);
    if (dialog_is_run()) {
        close_window(shown);
        shown = NULL;
    }
    if (mode_back >= 0) {
        if (cparam_i_get(cfg_cur_mode) == mode_forced) {    /* not changed again meanwhile */
            cparam_i_set(cfg_cur_mode, mode_back);
        }
        mode_back = -1;
    }
}

void dialog_send(lv_event_code_t event_code, void *param) {
    if (dialog_is_run()) {
        event_send(shown->obj, event_code, param);
    }
}

bool dialog_is_run() {
    return shown && shown->run;
}

bool dialog_type_is_run(dialog_t *dialog) {
    return shown == dialog && dialog_is_run();
}

/* The window's base object: the theme's window box, no scrolling */
lv_obj_t *dialog_init(lv_obj_t *parent) {
    lv_obj_t *win = lv_obj_create(parent);

    lv_obj_remove_style_all(win);
    lv_obj_add_style(win, &dialog_style, 0);
    lv_obj_clear_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    return win;
}

/* A control of the window: the theme's focus and edit looks, in the keyboard group, the
 * window's key handler */
void dialog_item(dialog_t *dialog, lv_obj_t *item) {
    lv_obj_add_style(item, &dialog_item_style, LV_STATE_DEFAULT);
    lv_obj_add_style(item, &dialog_item_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(item, &dialog_item_edited_style, LV_STATE_EDITED);

    /* text cursor: plain until edited, then half filled */
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_text_color(item, TC(text), LV_PART_CURSOR);
    lv_obj_set_style_text_color(item, TC(focus_text), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(item, 128, LV_PART_CURSOR | LV_STATE_EDITED);

    /* sliders and switches: their knob framed when reached by key */
    lv_obj_set_style_border_width(item, 2, LV_PART_INDICATOR | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(item, TC(focus), LV_PART_INDICATOR | LV_STATE_FOCUS_KEY);

    lv_obj_add_flag(item, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(keyboard_group, item);
    if (dialog->key_cb) {
        lv_obj_add_event_cb(item, dialog->key_cb, LV_EVENT_KEY, NULL);
    }
}

bool dialog_need_audio() {
    return dialog_is_run() && shown->audio_cb;
}

void dialog_audio_samples(unsigned int n, float *samples) {
    if (dialog_need_audio()) {
        shown->audio_cb(n, samples);
    }
}

void dialog_rotary(int32_t diff) {
    if (dialog_is_run() && shown->rotary_cb) {
        shown->rotary_cb(diff);
    }
}
