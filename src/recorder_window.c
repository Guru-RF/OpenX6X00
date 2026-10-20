/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - recorder screen: the recordings in a list, record, play, rename, delete
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "recorder_window.h"

#include "audio.h"
#include "button_bar.h"
#include "gui_events.h"
#include "key_input.h"
#include "radio.h"
#include "recorder.h"
#include "scheduler.h"
#include "gui_styles.h"
#include "text_popup.h"
#include "theme.h"

#include <dirent.h>
#include <pthread.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIST_W      775
#define LEVEL_H     25
#define PLAY_CHUNK  1024
#define SLOT_REC    1       /* Rec Stop alone on F2 while recording */
#define SLOT_PLAY   4       /* Play Stop alone on F5 while playing */

static lv_obj_t         *files_list;
static lv_obj_t         *meter;
static lv_timer_t       *meter_timer;
static int16_t          rows;
static volatile bool    playing;
static char             *renaming;      /* the name before Rename, while the editor is open */
static pthread_t        play_thr;
static int16_t          play_buf[PLAY_CHUNK];

static void open_cb(lv_obj_t *parent);
static void close_cb(void);
static void rec_cb(button_data_t *b);
static void rec_stop_cb(button_data_t *b);
static void rename_cb(button_data_t *b);
static void delete_cb(button_data_t *b);
static void play_cb(button_data_t *b);
static void play_stop_cb(button_data_t *b);

static button_data_t btn_rec       = { .type = BTN_TEXT, .label = "Rec",        .press = rec_cb };
static button_data_t btn_rec_stop  = { .type = BTN_TEXT, .label = "Rec\nStop",  .press = rec_stop_cb };
static button_data_t btn_rename    = { .type = BTN_TEXT, .label = "Rename",     .press = rename_cb };
static button_data_t btn_delete    = { .type = BTN_TEXT, .label = "Delete",     .press = delete_cb };
static button_data_t btn_play      = { .type = BTN_TEXT, .label = "Play",       .press = play_cb };
static button_data_t btn_play_stop = { .type = BTN_TEXT, .label = "Play\nStop", .press = play_stop_cb };

static buttons_page_t page = { { &btn_rec, &btn_rename, &btn_delete, &btn_play } };

static dialog_t dialog = { .construct_cb = open_cb, .destruct_cb = close_cb, .btn_page = &page };

dialog_t *dialog_recorder = &dialog;

/* The four buttons, or one alone in its slot while recording or playing */
static void set_buttons(button_data_t *alone, uint8_t slot) {
    buttons_unload_page();
    if (alone)
        buttons_load(slot, alone);
    else
        buttons_load_page(&page);
}

static void show_page(void *unused) {
    set_buttons(NULL, 0);
}

/* The files in the folder, in the order the folder gives them; one empty row if none */
static void list_files() {
    DIR             *dir = opendir(recorder_path);
    struct dirent   *de;

    rows = 0;
    if (dir) {
        while ((de = readdir(dir)))
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                lv_table_set_cell_value(files_list, rows++, 0, de->d_name);
        closedir(dir);
    }
    if (!rows)
        lv_table_set_cell_value(files_list, rows++, 0, "");
    lv_table_set_row_cnt(files_list, rows);
}

static const char *selected() {
    uint16_t r, c;

    if (!rows) return NULL;
    lv_table_get_selected_cell(files_list, &r, &c);
    return r == LV_TABLE_CELL_NONE ? NULL : lv_table_get_cell_value(files_list, r, c);
}

static void file_path(char *buf, size_t size, const char *name) {
    snprintf(buf, size, "%s/%s", recorder_path, name);
}

/* Plays the selected file to the end or until Play Stop, TX or leaving the screen */
static void *play_thread(void *arg) {
    const char  *name = NULL;
    char        path[300];
    SF_INFO     info = { 0 };
    SNDFILE     *f = NULL;
    bool        bt;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    /* on the Bluetooth headphones when they stand in for the speaker */
    bt = audio_play_bt_begin();
    if (!bt) radio_set_play_mode(AUDIO_PLAY_ON);

    name = selected();
    if (name) {
        file_path(path, sizeof(path), name);
        f = sf_open(path, SFM_READ, &info);
    }
    if (f) {
        if (recorder_is_on()) recorder_set_on(false);
        for (playing = true; playing; ) {
            int n = sf_read_short(f, play_buf, PLAY_CHUNK);

            if (n <= 0) break;
            audio_play(play_buf, n);
        }
        playing = false;
        sf_close(f);
        audio_play_wait();
    }

    if (bt) audio_play_bt_end();
    else radio_set_play_mode(AUDIO_PLAY_OFF);

    if (dialog.run) scheduler_put_noargs(show_page);
    return NULL;
}

/* The editor closed, with OK or not: the list takes the keys again */
static bool rename_closed() {
    lv_group_add_obj(keyboard_group, files_list);
    lv_group_set_editing(keyboard_group, true);
    free(renaming);
    renaming = NULL;
    return true;
}

static bool rename_ok() {
    const char  *name = textarea_window_get();
    char        from[300], to[300];

    if (strcmp(renaming, name)) {
        file_path(from, sizeof(from), renaming);
        file_path(to, sizeof(to), name);
        if (rename(from, to) == 0) list_files();
    }
    return rename_closed();     /* same name or a failed rename too */
}

/* Going to TX stops playing */
static void radio_tx_cb(lv_event_t *e) {
    if (!playing) return;
    playing = false;
    show_page(NULL);
}

static void meter_cb(lv_timer_t *t) {
    lv_bar_set_value(meter, audio_get_peak_db(), LV_ANIM_OFF);
}

static void list_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}

/* One column, no frame, the selected row in the focus colour; the knob moves the selection */
static lv_obj_t *make_list(lv_obj_t *parent) {
    lv_obj_t *t = lv_table_create(parent);

    lv_obj_remove_style(t, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(t, LIST_W, DIALOG_H - 26 - LEVEL_H);
    lv_table_set_col_cnt(t, 1);
    lv_table_set_col_width(t, 0, LIST_W - 5);

    lv_obj_set_style_border_width(t, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(t, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(t, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(t, 0, LV_PART_ITEMS);

    lv_obj_set_style_text_color(t, TC(focus_text), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_color(t, TC(focus), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(t, 128, LV_PART_ITEMS | LV_STATE_EDITED);

    lv_obj_add_event_cb(t, list_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, t);
    lv_group_set_editing(keyboard_group, true);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);
    return t;
}

/* The input level under the list, -60..0 dB, in the "on" colour */
static lv_obj_t *make_meter(lv_obj_t *parent, lv_obj_t *above) {
    static lv_style_t frame, fill;
    lv_obj_t *bar;

    lv_style_init(&frame);
    lv_style_set_border_color(&frame, TC(toggle_on));
    lv_style_set_border_width(&frame, 2);
    lv_style_set_pad_all(&frame, 6);
    lv_style_set_radius(&frame, 6);
    lv_style_set_anim_time(&frame, 1000);

    lv_style_init(&fill);
    lv_style_set_bg_opa(&fill, LV_OPA_COVER);
    lv_style_set_bg_color(&fill, TC(toggle_on));
    lv_style_set_radius(&fill, 3);

    bar = lv_bar_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &frame, 0);
    lv_obj_add_style(bar, &fill, LV_PART_INDICATOR);
    lv_obj_set_size(bar, LIST_W, LEVEL_H);
    lv_obj_align_to(bar, above, LV_ALIGN_OUT_BOTTOM_MID, 0, 7);
    lv_bar_set_range(bar, -60, 0);
    lv_bar_set_value(bar, -6, LV_ANIM_OFF);
    return bar;
}

static void open_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    show_page(NULL);
    lv_obj_add_event_cb(dialog.obj, radio_tx_cb, EVENT_RADIO_TX, NULL);

    files_list = make_list(dialog.obj);
    meter = make_meter(dialog.obj, files_list);
    meter_timer = lv_timer_create(meter_cb, 30, NULL);

    mkdir(recorder_path, 0755);
    list_files();
    if (recorder_is_on()) set_buttons(&btn_rec_stop, SLOT_REC);
}

static void close_cb(void) {
    radio_set_play_mode(AUDIO_PLAY_OFF);
    playing = false;
    textarea_window_close();
    lv_timer_del(meter_timer);
}

static void rec_cb(button_data_t *b) {
    recorder_set_on(true);
}

static void rec_stop_cb(button_data_t *b) {
    recorder_set_on(false);
    list_files();
}

static void play_cb(button_data_t *b) {
    pthread_create(&play_thr, NULL, play_thread, NULL);
    set_buttons(&btn_play_stop, SLOT_PLAY);
}

static void play_stop_cb(button_data_t *b) {
    playing = false;
}

static void rename_cb(button_data_t *b) {
    const char *name = selected();

    if (!name) return;
    renaming = strdup(name);
    lv_group_remove_obj(files_list);
    textarea_window_open(rename_ok, rename_closed);
    textarea_window_set(renaming);
}

static void delete_cb(button_data_t *b) {
    const char  *name = selected();
    char        path[300];

    if (!name) return;
    file_path(path, sizeof(path), name);
    unlink(path);
    list_files();
}

/* From recorder.c when recording starts or stops */
void dialog_recorder_set_on(bool on) {
    if (!dialog.run) return;

    set_buttons(on ? &btn_rec_stop : NULL, SLOT_REC);
    if (!on) list_files();
}
