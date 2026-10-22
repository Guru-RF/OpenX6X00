/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - GPS screen: satellites, fix, time, position, locator, gpsd state
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "gps_window.h"

#include "gps.h"
#include "gui_styles.h"
#include "gui_events.h"
#include "radio.h"
#include "key_input.h"
#include "qth/qth.h"

#include <math.h>
#include <stdio.h>

/* Seven rows in the dialog's height, the values in one column wide enough for a longitude */
#define ROWS        7
#define ROW_H       ((DIALOG_H - 12) / ROWS)
#define NAME_X      30
#define NAME_W      340
#define VALUE_X     (NAME_X + NAME_W + 10)
#define VALUE_W     (796 - VALUE_X - 20)

enum { R_SATS, R_FIX, R_TIME, R_LAT, R_LON, R_GRID, R_STATUS };

static const char *names[ROWS] = {
    "Sat in view/in use:", "Fix:", "Date, time:", "Latitude:", "Longitude:", "QTH Grid:", "GPS status"
};

static lv_obj_t     *values[ROWS];
static lv_timer_t   *status_timer;

static void open_cb(lv_obj_t *parent);
static void close_cb(void);
static void window_key_cb(lv_event_t *e);

static dialog_t dialog = { .construct_cb = open_cb, .destruct_cb = close_cb, .key_cb = window_key_cb };

dialog_t *dialog_gps = &dialog;

/* Degrees and minutes to the micro-minute, "45 N 04.274070'"; N/A past a full turn */
static void coord_text(char *buf, size_t size, double v, const char *pos, const char *neg) {
    const char  *side = v < 0 ? neg : pos;
    double      x;
    int         deg;

    if (!isfinite(v) || fabs(v) > 360.0) {
        snprintf(buf, size, "N/A");
        return;
    }
    x = fabs(v) + 0.5e-6 / 60;          /* rounded to the last digit shown */
    deg = (int)x;
    x = (x - deg) * 60;
    if (deg == 360) {
        deg = 0;
        x = 0;
    }
    int min = (int)x;

    snprintf(buf, size, "%d %s %02d.%06d'", deg, side, min, (int)((x - min) * 1000000.0));
}

/* One report from gpsd (gps.c); the position rows only with a 2D or 3D fix */
static void report_cb(lv_event_t *e) {
#if HAVE_LIBGPS
    struct gps_data_t   *g = lv_event_get_param(e);
    char                text[64];

    if (g->set & SATELLITE_SET)
        lv_label_set_text_fmt(values[R_SATS], "%i/%i", g->satellites_visible, g->satellites_used);

    switch (g->fix.mode) {
        case MODE_3D:       lv_label_set_text(values[R_FIX], "3D");     break;
        case MODE_2D:       lv_label_set_text(values[R_FIX], "2D");     break;
        case MODE_NO_FIX:   lv_label_set_text(values[R_FIX], "None");   break;
        default:            break;
    }

    if (g->set & TIME_SET) {
        timespec_to_iso8601(g->fix.time, text, sizeof(text));
        lv_label_set_text(values[R_TIME], text);
    } else {
        lv_label_set_text(values[R_TIME], "N/A");
    }

    if (g->fix.mode < MODE_2D) {
        lv_label_set_text(values[R_LAT], "N/A");
        lv_label_set_text(values[R_LON], "N/A");
        lv_label_set_text(values[R_GRID], "N/A");
        return;
    }
    coord_text(text, sizeof(text), g->fix.latitude, "N", "S");
    lv_label_set_text(values[R_LAT], text);
    coord_text(text, sizeof(text), g->fix.longitude, "E", "W");
    lv_label_set_text(values[R_LON], text);
    qth_pos_to_str(g->fix.latitude, g->fix.longitude, text);
    lv_label_set_text(values[R_GRID], text);
#endif
}

static void status_cb(lv_timer_t *t) {
    static const char *state[] = { "waiting", "working", "restarting", "exited" };

    lv_label_set_text(values[R_STATUS], state[gps_status()]);
}

/* A name on the left, its value on the right; the status row starts empty, the rest N/A */
static void add_row(lv_obj_t *win, int i) {
    lv_coord_t  y = 6 + i * ROW_H;
    lv_obj_t    *name = lv_label_create(win);

    lv_label_set_text(name, names[i]);
    lv_obj_set_size(name, NAME_W, ROW_H);
    lv_obj_set_pos(name, NAME_X, y);

    values[i] = lv_label_create(win);
    lv_label_set_text(values[i], i == R_STATUS ? "" : "N/A");
    lv_obj_set_size(values[i], VALUE_W, ROW_H);
    lv_obj_set_pos(values[i], VALUE_X, y);
}

static void open_cb(lv_obj_t *parent) {
    lv_obj_t *win = dialog_init(parent);

    dialog.obj = win;
    lv_obj_set_style_text_font(win, &sony_28, 0);
    for (int i = 0; i < ROWS; i++)
        add_row(win, i);

    lv_obj_add_event_cb(win, report_cb, EVENT_GPS, NULL);
    lv_obj_add_event_cb(win, window_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, win);

    /* gpsd's state twice a second, the first one now */
    status_timer = lv_timer_create(status_cb, 500, NULL);
    lv_timer_ready(status_timer);
}

static void close_cb(void) {
    lv_timer_del(status_timer);
}

static void window_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}
