/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - GPS / Clock screen: satellites, fix, time, position, locator, gpsd state; the clock, what
 *  keeps it or moved it last, what the FT8 decodes say of it, and a time station's minute marker, with the one switch
 *  that lets FT8 move it (Auto Sync, clock_sync.h). The clock part from OlivLoze's fork (F4OLI)
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "gps_window.h"
#include "voice.h"
#include "voice_focus.h"

#include "gps.h"
#include "gui_styles.h"
#include "gui_events.h"
#include "radio.h"
#include "key_input.h"
#include "qth/qth.h"
#include "clock_sync.h"
#include "time_station.h"
#include "freq_list.h"
#include "msg_box.h"
#include "i18n.h"
#include "cfg/cfg_api.h"
#include "ft8/time_sync.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Seven rows in the dialog's height, the values in one column wide enough for a position */
#define ROWS        7
#define ROW_H       ((DIALOG_H - 12) / ROWS)
#define NAME_X      20
#define NAME_W      220
#define VALUE_X     (NAME_X + NAME_W + 10)
#define VALUE_W     (DIALOG_W - VALUE_X - 10)

#define FT8_WORD_AGE_S  600     /* FT8's word older than this is none */
#define STATION_RIGHT_S 0.02f   /* a time station saying the clock is this close: not moved */

enum { R_SATS, R_TIME, R_POS, R_GRID, R_CLOCK, R_FT8, R_STATION };

static const char *names[ROWS] = {
    "Satellites, fix:", "GPS date, time:", "Position:", "Grid, gpsd:", "Clock:", "FT8 says:", "Time station:"
};

static lv_obj_t     *values[ROWS];
static lv_timer_t   *status_timer;
static char          grid[16] = "N/A";
static int           station = 1;       /* RWM 9996: Moscow, heard all over Europe */
static bool          tuned;             /* the radio given back on close */
static unsigned      result_seen;

static void open_cb(lv_obj_t *parent);
static void close_cb(void);
static void window_key_cb(lv_event_t *e);

static const char *station_label(void);
static const char *listen_label(void);
static const char *auto_sync_label(void);
static void station_cb(button_data_t *btn);
static void listen_cb(button_data_t *btn);
static void auto_sync_cb(button_data_t *btn);

static button_data_t btn_station   = {.type = BTN_TEXT_FN, .label_fn = station_label, .press = station_cb};
static button_data_t btn_listen    = {.type = BTN_TEXT_FN, .label_fn = listen_label, .press = listen_cb};
static button_data_t btn_auto_sync = {.type = BTN_TEXT_FN, .label_fn = auto_sync_label, .press = auto_sync_cb};

static buttons_page_t page = {{&btn_station, &btn_listen, &btn_auto_sync}};

static dialog_t dialog = {
    .construct_cb = open_cb, .destruct_cb = close_cb, .key_cb = window_key_cb, .btn_page = &page
};

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

/* One report from gpsd (gps.c); the satellites its last count, once gpsd gave one; the position rows only with a 2D
 * or 3D fix */
static void report_cb(lv_event_t *e) {
#if HAVE_LIBGPS
    struct gps_data_t   *g = lv_event_get_param(e);
    char                text[64], lat[24], lon[24];
    const char          *fix = g->fix.mode == MODE_3D ? "3D" : g->fix.mode == MODE_2D ? "2D" : _("no fix");

    if (g->set & SATELLITE_SET)
        lv_label_set_text_fmt(values[R_SATS], "%i/%i, %s", g->satellites_visible, g->satellites_used, fix);

    if (g->set & TIME_SET) {
        timespec_to_iso8601(g->fix.time, text, sizeof(text));
        lv_label_set_text(values[R_TIME], text);
    } else {
        lv_label_set_text(values[R_TIME], "N/A");
    }

    {
        /* Spoken: a fix found or lost ("GPS fix, 3D, grid F N 4 2"), once each time it changes */
        static int said = -1;
        int        now  = g->fix.mode >= MODE_2D ? (int)g->fix.mode : 0;

        if (said >= 0 && now != said) {
            if (now) {
                char grid[16], spelled[48];

                qth_pos_to_str(g->fix.latitude, g->fix.longitude, grid);
                voice_spell_calls(grid, spelled, sizeof(spelled));
                voice_say_text_fmt("GPS fix, %s, grid %s", now == MODE_3D ? "3D" : "2D", spelled);
            } else {
                voice_say_text_fmt("GPS fix lost");
            }
        }
        said = now;
    }
    if (g->fix.mode < MODE_2D) {
        lv_label_set_text(values[R_POS], "N/A");
        snprintf(grid, sizeof(grid), "N/A");
        return;
    }
    coord_text(lat, sizeof(lat), g->fix.latitude, "N", "S");
    coord_text(lon, sizeof(lon), g->fix.longitude, "E", "W");
    lv_label_set_text_fmt(values[R_POS], "%s  %s", lat, lon);
    qth_pos_to_str(g->fix.latitude, g->fix.longitude, grid);
#endif
}

/* "3 min ago" */
static void ago(char *buf, size_t size, time_t when) {
    long s = (long)(time(NULL) - when);

    if (s < 90)
        snprintf(buf, size, _("%ld s ago"), s < 0 ? 0 : s);
    else if (s < 90 * 60)
        snprintf(buf, size, _("%ld min ago"), s / 60);
    else
        snprintf(buf, size, _("%ld h ago"), s / 3600);
}

/* The clock and what keeps it or moved it last */
static void clock_row(void) {
    time_t      now = time(NULL), when;
    struct tm   utc;
    char        name[32], a[24];
    double      step;
    clock_by_t  by;
    int         err_ms;
    clock_by_t  keeper = clock_sync_keeper(&err_ms);

    gmtime_r(&now, &utc);
    if (keeper == CLOCK_BY_NTP && err_ms)
        lv_label_set_text_fmt(values[R_CLOCK], "%02d:%02d:%02d UTC, kept by NTP (%d ms)", utc.tm_hour, utc.tm_min,
                              utc.tm_sec, err_ms);
    else if (keeper != CLOCK_BY_NONE)
        lv_label_set_text_fmt(values[R_CLOCK], "%02d:%02d:%02d UTC, kept by %s", utc.tm_hour, utc.tm_min, utc.tm_sec,
                              _(clock_sync_by_name(keeper)));
    else if (clock_sync_last(&by, name, sizeof(name), &when, &step)) {
        ago(a, sizeof(a), when);
        lv_label_set_text_fmt(values[R_CLOCK], "%02d:%02d:%02d UTC, %s %+.2f s %s", utc.tm_hour, utc.tm_min,
                              utc.tm_sec, _(name), step, a);     /* "phone", "Time Sync"; a station's stays */
    } else {
        lv_label_set_text_fmt(values[R_CLOCK], "%02d:%02d:%02d UTC, clock chip", utc.tm_hour, utc.tm_min, utc.tm_sec);
    }
}

/* The FT8 decodes' word on the clock, given whenever FT8 or FT4 is open (Auto Sync on or off) */
static void ft8_row(void) {
    clock_ft8_word_t w;
    char             a[24];

    clock_sync_ft8_get(&w);
    if (!w.valid || time(NULL) - w.when > FT8_WORD_AGE_S) {
        lv_label_set_text(values[R_FT8], "no word: FT8 or FT4 open on a busy band");
        return;
    }
    ago(a, sizeof(a), w.when);
    switch (w.rc) {
        case FTX_SYNC_ON_TIME:
            lv_label_set_text_fmt(values[R_FT8], "on time (%+.2f s), %d stations, %s", -w.step, w.stations, a);
            break;
        case FTX_SYNC_STEP:
            lv_label_set_text_fmt(values[R_FT8], "%.2f s %s, %d stations, %s", fabsf(w.step),
                                  w.step < 0 ? _("ahead") : _("behind"), w.stations, a);
            break;
        case FTX_SYNC_TOO_FAR:
            lv_label_set_text_fmt(values[R_FT8], "%.1f s %s: too far, Time Sync in FT8", fabsf(w.step),
                                  w.step < 0 ? _("ahead") : _("behind"));
            break;
        case FTX_SYNC_SPREAD:
            lv_label_set_text_fmt(values[R_FT8], "the stations disagree (%d), %s", w.stations, a);
            break;
        default:
            lv_label_set_text_fmt(values[R_FT8], "listening: %d stations, %d slots", w.stations, w.slots);
            break;
    }
}

/* A time station: what it heard; two markers that agree move the clock, or teach the delay while it is kept */
static void station_row(void) {
    time_station_state_t  st;
    const time_station_t *ts;
    char                  a[24];
    clock_by_t            keeper;

    time_station_get(&st);
    ts = &time_stations[st.on ? st.station : station];
    if (!st.on) {
        lv_label_set_text_fmt(values[R_STATION], "%s: off", ts->name);
        return;
    }
    if (st.result && st.result_id != result_seen) {
        float delay = clock_sync_station_delay(NULL), ahead = -st.raw - delay;

        result_seen = st.result_id;
        if (!clock_sync_allowed(CLOCK_BY_STATION, &keeper)) {
            clock_sync_station_delay_learn(-st.raw);
            msg_update_text_fmt("%s keeps the clock\nThe marker's delay learned: %.3f s", _(clock_sync_by_name(keeper)),
                                -st.raw);
        } else if (fabsf(ahead) < STATION_RIGHT_S) {
            msg_update_text_fmt("Clock right by %s\n%+.3f s", ts->name, ahead);
        } else if (clock_sync_step(-ahead, CLOCK_BY_STATION, ts->name)) {
            msg_update_text_fmt("Clock moved %+.3f s\nby %s", -ahead, ts->name);
        }
    }

    int minute = (int)(time(NULL) / 60 % 60), next = time_station_quiet_until(ts, minute);

    if (!st.result && next >= 0) {
        lv_label_set_text_fmt(values[R_STATION], "%s: no markers until :%02d", ts->name, next);
        return;
    }
    if (!st.markers) {
        lv_label_set_text_fmt(values[R_STATION], "%s: listening", ts->name);
        return;
    }
    ago(a, sizeof(a), (time_t)st.marker_at);
    if (st.result) {
        float ahead = -st.raw - clock_sync_station_delay(NULL);

        lv_label_set_text_fmt(values[R_STATION], "%s: %s %.3f s, %.0f dB", ts->name,
                              ahead >= 0 ? _("ahead") : _("behind"), fabsf(ahead), st.snr_db);
    } else {
        lv_label_set_text_fmt(values[R_STATION], "%s: marker %s, %.0f dB", ts->name, a, st.snr_db);
    }
}

static void status_cb(lv_timer_t *t) {
    static const char *state[] = { N_("waiting"), N_("working"), N_("restarting"), N_("exited") };

    lv_label_set_text_fmt(values[R_GRID], "%s, %s", grid, _(state[gps_status()]));
    clock_row();
    ft8_row();
    station_row();
}

/* A name on the left, its value on the right */
static void add_row(lv_obj_t *win, int i) {
    lv_coord_t  y = 6 + i * ROW_H;
    lv_obj_t    *name = lv_label_create(win);

    lv_label_set_text(name, names[i]);
    lv_obj_set_size(name, NAME_W, ROW_H);
    lv_obj_set_pos(name, NAME_X, y);

    values[i] = lv_label_create(win);
    lv_label_set_text(values[i], "N/A");
    lv_label_set_long_mode(values[i], LV_LABEL_LONG_DOT);
    lv_obj_set_size(values[i], VALUE_W, ROW_H);
    lv_obj_set_pos(values[i], VALUE_X, y);
}

/* ---- buttons ---- */

static const char *station_label(void) {
    static char text[32];

    snprintf(text, sizeof(text), "Station:\n%s", time_stations[station].name);
    return text;
}

static const char *listen_label(void) {
    return dialog.audio_cb ? "Stop\nlistening" : "Listen";
}

static const char *auto_sync_label(void) {
    return param_i_get(cfg_ft8_auto_sync) ? "Auto Sync:\nOn" : "Auto Sync:\nOff";
}

/* The station's marker as its tone in USB; frequency and mode back when the window closes */
static void tune(void) {
    const time_station_t *s  = &time_stations[station];
    freq_list_item_t      it = {.freq_hz = s->freq_hz, .name = s->name};

    if (!tuned) {
        dialog_restore_radio_on_close();
        tuned = true;
    }
    freq_list_tune(&it, s->freq_hz - s->dial_hz);
}

static void listen_start(void) {
    tune();
    time_station_start(station);
    dialog.audio_cb = time_station_audio;
}

static void listen_stop(void) {
    dialog.audio_cb = NULL;
    time_station_stop();
}

static void station_cb(button_data_t *btn) {
    station = (station + 1) % time_stations_n;
    buttons_refresh(&btn_station);
    if (dialog.audio_cb)
        listen_start();
}

static void listen_cb(button_data_t *btn) {
    if (dialog.audio_cb)
        listen_stop();
    else
        listen_start();
    buttons_refresh(&btn_listen);
}

/* Auto Sync: FT8 and FT4 move the clock when nothing keeps it (clock_sync.h's rule); off, they only measure */
static void auto_sync_cb(button_data_t *btn) {
    bool       on = !param_i_get(cfg_ft8_auto_sync);
    clock_by_t keeper;

    param_i_set(cfg_ft8_auto_sync, on);
    buttons_refresh(&btn_auto_sync);
    if (!on) {
        msg_update_text_fmt("Auto Sync off\nFT8 only measures the clock");
        return;
    }
    clock_sync_allowed(CLOCK_BY_FT8, &keeper);
    if (keeper != CLOCK_BY_NONE)
        msg_update_text_fmt("Auto Sync on\n%s keeps the clock for now", _(clock_sync_by_name(keeper)));
    else
        msg_update_text_fmt("Auto Sync on\nFT8 and FT4 set the clock");
}

static void open_cb(lv_obj_t *parent) {
    lv_obj_t *win = dialog_init(parent);

    dialog.obj = win;
    tuned      = false;
    lv_obj_set_style_text_font(win, &sony_24, 0);
    snprintf(grid, sizeof(grid), "N/A");
    for (int i = 0; i < ROWS; i++)
        add_row(win, i);

    lv_obj_add_event_cb(win, report_cb, EVENT_GPS, NULL);
    lv_obj_add_event_cb(win, window_key_cb, LV_EVENT_KEY, NULL);
    voice_focus_quiet(win);     /* the knob reads it all (window_key_cb) */
    lv_group_add_obj(keyboard_group, win);

    /* gpsd's state and the clock twice a second, the first one now */
    status_timer = lv_timer_create(status_cb, 500, NULL);
    lv_timer_ready(status_timer);
}

static void close_cb(void) {
    listen_stop();
    lv_timer_del(status_timer);
}

static void window_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN ||
             key == LV_KEY_ENTER)
        voice_focus_read_as("GPS", dialog.obj);     /* the small knob: every row read */
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}
