// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *  Xiegu X6100 LVGL GUI - the FT8 / FT4 / FT2 window
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  Waterfall with the TX frequency marker, the message list over it (ft8/table_view.c), four
 *  pages of buttons. Decoding runs in ft8/audio_worker.c; its callbacks below run on that
 *  thread and reach LVGL only through scheduler_put(). The QSO sequence is ft8/qso.cpp's,
 *  DXpedition Hound mode ft8/hound.c's, the transmission ft8/tx_worker.c's.
 */

#include "ft8_window.h"
#include "board.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "adif.h"
#include "audio.h"
#include "button_bar.h"
#include "cfg/cfg_api.h"
#include "cfg/digital_modes.h"
#include "cluster/client.h"
#include "app_window.h"
#include "dialog_field.h"
#include "dsp.h"
#include "dxcc.h"
#include "gui_events.h"
#include "flags.h"
#include "freq_strip.h"
#include "gps.h"
#include "ft8_udp.h"
#include "js8_map.h"
#include "key_input.h"
#include "main.h"
#include "screen_main.h"
#include "msg_box.h"
#include "params/params.h"
#include "pskreporter.h"
#include "qso_log.h"
#include "qth/qth.h"
#include "radio.h"
#include "scheduler.h"
#include "station.h"
#include "gui_styles.h"
#include "text_popup.h"
#include "theme.h"
#include "tx_player.h"
#include "util.h"
#include "btaudio.h"   /* sh_run */
#include "ft8/audio_worker.h"
#include "ft8/cq_scheduler.h"
#include "ft8/ft2.h"
#include "ft8/hound.h"
#include "ft8/qso.h"
#include "ft8/decode_list.h"
#include "ft8/tx_worker.h"
#include "ft8/utils.h"
#include "ft8/worker.h"
#include "widgets/tune_marker.h"
#include "widgets/app_waterfall.h"

#define SAMPLE_RATE     (AUDIO_CAPTURE_RATE / AUDIO_DECIM)

#define WIDTH           771
#define WF_HEIGHT       (DIALOG_H - 26)   /* 13 px margins above and below */
#define WF_VISIBLE      40     /* waterfall clear above the log: ~16 s, a slot, to find a clear spot */

#define CUSTOM_MIN_HZ   1800000  /* custom dial frequency: 160m ... */
#define CUSTOM_MAX_HZ   54000000 /* ... to the top of 6m */


/* ---- FT8 / FT4 / FT2 (FT2 from SP2KMO's upstream 76d98ce2) ---------------
 * cfg_ft8_protocol: FTX_PROTOCOL_FT4 (0), FTX_PROTOCOL_FT8 (1) or FTX_PROTOCOL_FT2 (2, ft8/ft2.h) */

typedef struct {
    int                width_hz;    /* the TX marker: the signal's bandwidth */
    cfg_digital_type_t presets;     /* its dial frequencies in digital_modes */
    qso_log_mode_t     log_mode;
} proto_info_t;

static const proto_info_t protos[] = {
    [FTX_PROTOCOL_FT4] = {83,  CFG_DIG_TYPE_FT4, MODE_FT4},
    [FTX_PROTOCOL_FT8] = {50,  CFG_DIG_TYPE_FT8, MODE_FT8},
    [FTX_PROTOCOL_FT2] = {167, CFG_DIG_TYPE_FT2, MODE_FT2},
};

static int cur_proto(void) {
    return param_i_get(cfg_ft8_protocol);
}

/* Any other value counts as FT2 */
static const proto_info_t *proto(void) {
    int p = cur_proto();
    return &protos[p == FTX_PROTOCOL_FT8 || p == FTX_PROTOCOL_FT4 ? p : (int)FTX_PROTOCOL_FT2];
}

static const char *mode_name(void) {
    return ftx_protocol_name(cur_proto());
}

static bool is_ft2(void) {
    return cur_proto() == (int)FTX_PROTOCOL_FT2;
}

/* A transmission may start this late into its slot: FT2 lasts 2.52 s and must end inside 3.75 s */
static float tx_start_limit(void) {
    return is_ft2() ? 0.8f : 1.5f;
}

/* ---- state ---- */

static dialog_t dialog;

/* The window's instance (audit C06): bumped as it closes, once the decoder thread is joined. A
 * job that thread queued for the window carries the instance it was made in; one that comes after
 * its window closed, or in the window opened since, is dropped, its payload freed. The scheduler
 * keeps a job after the join: the join retires the thread, not what it queued */
static uint32_t win_gen;

static SubjectInt   *tx_enabled;    /* TX Call */
static SubjectInt   *cq_enabled;    /* TX CQ */

/* The QSO sequence (audit C05): the processor (qso), the next transmission and Hound's state change
 * on the decoder thread (a decode, a slot's end, a transmission) and on the GUI (a key, a row, the
 * browser panel; the panel's status reads them on its own thread). Each takes qso_mux for a whole
 * step, so a reset on the GUI can't delete the QSO the decoder is completing, nor a decode answer
 * over a call just chosen. Never held across a wait for another thread: not over a transmission,
 * not for a GUI job (gui_post may wait for room), not while the GUI joins the decoder. A QSO
 * completed in a step is saved after it, outside (qso_unlock) */
static pthread_mutex_t qso_mux = PTHREAD_MUTEX_INITIALIZER;
static bool            tx_odd;      /* the slot (odd or even) our transmissions go in */
static ftx_tx_msg_t    tx_msg;      /* the next transmission, "" none */
/* Bumped by each change of the next transmission from the GUI (a key, a row, the panel, a restart):
 * what a decoder step queued for the GUI before it is out of date (the step answered a station, the
 * operator called CQ since) */
static uint32_t        tx_gen;
/* Our transmission is on: set under qso_mux as transmit() takes the message, cleared under it by
 * each stop (TX CQ off, TX Call off, Hound, the panel's STOP, decoder_stop), so a stop after the
 * message was taken still ends it (tx_stopped_cb). Read anywhere */
static atomic_bool     sending;
/* The decoder runs for this window: off from the start of decoder_stop, so that what the thread
 * still does before its join (a last decode answered, the slot for it) sends nothing */
static bool            decoder_on;

/* save_qso() runs on the decoder thread and on the GUI (Force QSO save): one at a time, so that two
 * records don't mix in the ADIF file; the last QSO saved goes with them (the panel's status reads
 * it). Never taken with qso_mux held, nor held over a GUI job */
static pthread_mutex_t save_mux = PTHREAD_MUTEX_INITIALIZER;
static char            last_logged[16];     /* the last QSO saved (the panel's timeline), "" none */
static time_t          last_logged_at;

/* UTC start of the slot being received, "HH:MM:SS": in front of each row of it (worker thread) */
static char          slot_time[9] = "--:--:--";

/* DXpedition Hound mode (FT8 only, off each time the app opens): under qso_mux, the GUI's own
 * reads of hound_on aside (it is the only one to set it) */
static bool    hound_on;
static hound_t hound;

static lv_obj_t *waterfall;
static lv_obj_t *finder;            /* the TX frequency marker over the waterfall */
static lv_obj_t *map_obj;           /* QSO or heard map, when open */
static lv_obj_t *freq_list;         /* Freq: the presets, when open */

/* The list fades out while the main knob moves the TX frequency, back a second after */
static lv_anim_t   fade;
static bool        fading;
static lv_timer_t *fade_back_timer;

static bool kb_open;                /* a keyboard window: the buttons wait */
static bool kb_freq;                /* that keyboard is for the dial frequency, else the CQ modifier */

static audio_worker_t  *audio_worker;
static FTxQsoProcessor *qso;
static adif_log         ft8_log;

static double  my_lat, my_lon;
static int32_t filter_low, filter_high;
static float   base_gain_offset;

static button_data_t button_freq, button_hound;

static void add_info(const char *fmt, ...);
static void list_pressed(const cell_data_t *cd);
static bool kb_ok_cb(void);
static bool kb_cancel_cb(void);
static void kb_close(void);

/* A row for the list, as a job of this window (win_gen): the decoder's come after it closed too */
typedef struct {
    cell_data_t cd;
    uint32_t    win_gen;
} row_job_t;

static void row_apply(void *arg) {
    row_job_t *j = arg;

    if (j->win_gen == win_gen) table_view_add_msg_cb(&j->cd);
}

static void row_post(const cell_data_t *cd) {
    row_job_t j = {.cd = *cd, .win_gen = win_gen};

    scheduler_put(row_apply, &j, sizeof(j));
}

static void give_list_the_mfk(void) {
    lv_group_add_obj(keyboard_group, table_view_obj());
    lv_group_focus_obj(table_view_obj());
    lv_group_set_editing(keyboard_group, true);
}

/* ---- WSJT-X's UDP messages for loggers (GridTracker, JTAlert...): ft8_udp.c, after SP2KMO's
 * upstream PR #275. A status goes out when something they show changes (RX/TX, the audio offset,
 * the dial, the station in QSO), checked every 250 ms, and with the heartbeat every 15 s as
 * WSJT-X sends it */

#define UDP_CHECK_MS    250
#define UDP_BEAT_CHECKS 60

static lv_timer_t *udp_check_timer;
static uint32_t    udp_checks;
static struct {
    int     sending;
    int32_t df;
    int64_t dial;
    char    dx[16];
} udp_sent = {-1, -1, -1};

static void qso_lock(void);
static void qso_unlock(void);

/* The station in QSO: the first word of the next transmission ("DXCALL MYCALL ..."), none for a CQ */
static void udp_dx_call(char dx[16]) {
    qso_lock();
    snprintf(dx, 16, "%.*s", (int)strcspn(tx_msg.msg, " "), tx_msg.msg);
    qso_unlock();
    if (!strcmp(dx, "CQ")) dx[0] = '\0';
}

static void udp_status(void) {
    char dx[16];

    udp_dx_call(dx);
    ft8_udp_send_status((uint64_t)cparam_i_get(cfg_fg_freq), mode_name(), params.callsign.x, station_qth(), dx,
                        params.ft8_tx_freq.x, params.ft8_tx_freq.x, sending, !sending);
    udp_sent.sending = sending;
    udp_sent.df      = params.ft8_tx_freq.x;
    udp_sent.dial    = cparam_i_get(cfg_fg_freq);
    snprintf(udp_sent.dx, sizeof(udp_sent.dx), "%s", dx);
}

static void udp_check_cb(lv_timer_t *t) {
    if (++udp_checks >= UDP_BEAT_CHECKS) {
        udp_checks = 0;
        ft8_udp_send_heartbeat();
    } else {
        char dx[16];

        udp_dx_call(dx);
        if (sending == udp_sent.sending && params.ft8_tx_freq.x == udp_sent.df &&
            cparam_i_get(cfg_fg_freq) == udp_sent.dial && !strcmp(dx, udp_sent.dx))
            return;
    }
    udp_status();
}

/* ---- the log ---- */

/* A QSO is complete (qso.cpp, or the Fox's RR73): the ADIF file, the log, the loggers, the remote */
/* ---- The decoder thread's hands on the screen (audit B05) ----
 * on_message and on_tick run on the audio worker: what they want of the GUI (the finder's cursor
 * on and off, its TX frequency, CQ off, the Field Ops entry of a logged QSO) goes to the LVGL
 * thread as a job.
 * A full queue is tried again for a while, never waited on for good: the GUI joins this thread
 * when the window closes */
static void set_tx_freq(int32_t hz);

static void gui_post(scheduler_fn_t fn, const void *arg, size_t n) {
    for (int i = 0; i < 100; i++) {
        if (scheduler_put(fn, (void *)arg, n)) return;
        usleep(20000);
    }
    if (!scheduler_put_urgent(fn, (void *)arg, n)) LV_LOG_ERROR("FT8: the GUI queue stayed full, a job was dropped");
}

typedef struct {
    bool     cursor_off, cursor, txfreq, cq_off;
    float    cursor_hz;
    uint32_t tx_gen;    /* of the step that made it */
    uint32_t win_gen;
} ui_job_t;

static void ui_apply(void *arg) {
    ui_job_t *j = arg;

    if (j->win_gen != win_gen) return;  /* its window closed meanwhile */
    /* The TX frequency is the parameter already (the transmission reads it), set by the decoder
     * or by the operator since: the marker and the strip show it as it is now */
    if (j->txfreq) set_tx_freq(params.ft8_tx_freq.x);
    /* The station answered, a CQ over: not when the operator chose another transmission since */
    if (j->tx_gen != tx_gen) return;
    if (j->cursor_off) lv_finder_clear_cursor(finder);
    if (j->cursor) lv_finder_set_cursor(finder, j->cursor_hz);
    if (j->cq_off && subject_i_get(cq_enabled)) subject_i_set(cq_enabled, false);
}

static void ui_post(const ui_job_t *j) {
    ui_job_t k = *j;

    k.win_gen = win_gen;
    gui_post(ui_apply, &k, sizeof(k));
}

/* The TX frequency from the worker: the parameter now (the transmission reads it), the marker
 * and the strip through the job */
static void ui_job_tx_freq(ui_job_t *j, int32_t hz) {
    hz = LV_MAX(hz, filter_low);     /* as set_tx_freq clamps */
    params_uint16_set(&params.ft8_tx_freq, hz);
    j->txfreq = true;
}

/* A QSO the sequence completed, noted under qso_mux (qso_completed) */
typedef struct {
    char     call[32];
    char     grid[16];
    int      rcvd, sent;
    uint32_t tx_gen;    /* of the step that completed it */
} done_qso_t;

/* GUI thread: the QSO into the Field Ops log (its screen follows) whether the window is open or
 * not, the finder's cursor off as a job of the window */
typedef struct {
    qso_log_record_t rec;
    ui_job_t         ui;
} saved_job_t;

static void save_qso_apply(void *arg) {
    saved_job_t *j = arg;

    dialog_field_day_log(&j->rec);
    ui_apply(&j->ui);
}

/* A QSO the sequence completed (the decoder thread, or the Save key on the GUI), after its step:
 * the ADIF and the log database here, the Field Ops entry and the cursor on the GUI thread */
static void save_qso(const done_qso_t *d) {
    const char *call = d->call, *grid = d->grid;
    const int   rcvd = d->rcvd, sent = d->sent;
    time_t now = time(NULL);
    int64_t dial = cparam_i_get(cfg_fg_freq);
    char  *canon = util_canonize_callsign(call, false);

    qso_log_record_t rec = qso_log_record_create(params.callsign.x, canon, now, proto()->log_mode, sent, rcvd,
                                                 dial, NULL, NULL, station_qth(), grid);
    free(canon);

    pthread_mutex_lock(&save_mux);
    adif_add_qso(ft8_log, rec);
    qso_log_record_save(rec);
    snprintf(last_logged, sizeof(last_logged), "%s", call);
    last_logged_at = now;
    pthread_mutex_unlock(&save_mux);

    saved_job_t job = {.rec = rec, .ui = {.cursor_off = true, .tx_gen = d->tx_gen, .win_gen = win_gen}};

    gui_post(save_qso_apply, &job, sizeof(job));
    ft8_udp_log_qso(now, rec.remote_call, grid, (uint64_t)cparam_i_get(cfg_fg_freq), mode_name(), sent, rcvd,
                    (int)param_f_get(cfg_pwr), params.callsign.x, station_qth());

    if (strlen(grid) >= 4) {
        double lat, lon;
        qth_str_to_pos(grid, &lat, &lon);
        msg_schedule_long_text_fmt("Saved QSO de %s %d %d (%.0f km)", call, sent, rcvd,
                                   qth_pos_dist(lat, lon, my_lat, my_lon));
    } else {
        msg_schedule_long_text_fmt("Saved QSO de %s %d %d", call, sent, rcvd);
    }
}

/* The QSO completed in a step under qso_mux, for qso_unlock(): the processor's, or in Hound mode
 * Hound's own (add_rx_text drops the processor's there) */
static done_qso_t done_qsos[1];
static int        done_n;

/* The processor's save callback, and Hound's RR73: under qso_mux, so noted only. The candidate
 * that called it may be gone once the lock is free */
static void qso_completed(const char *call, const char *grid, const int rcvd, const int sent) {
    if (done_n == (int)ARRAY_SIZE(done_qsos)) {
        LV_LOG_ERROR("FT8: a QSO with %s completed, no room to save it", call);
        return;
    }
    done_qso_t *d = &done_qsos[done_n++];

    snprintf(d->call, sizeof(d->call), "%s", call);
    snprintf(d->grid, sizeof(d->grid), "%s", grid);
    d->rcvd   = rcvd;
    d->sent   = sent;
    d->tx_gen = tx_gen;
}

static void qso_lock(void) {
    pthread_mutex_lock(&qso_mux);
}

/* The step's completed QSOs are saved after the lock: their logging and GUI jobs may wait */
static void qso_unlock(void) {
    done_qso_t done[ARRAY_SIZE(done_qsos)];
    int        n = done_n;

    memcpy(done, done_qsos, n * sizeof(done[0]));
    done_n = 0;
    pthread_mutex_unlock(&qso_mux);
    for (int i = 0; i < n; i++) save_qso(&done[i]);
}

/* Activators spotted on POTA now: the cluster store, which x6100-pota-spots feeds (worker thread only) */
static bool pota_spotted(const char *call) {
    static cluster_spot_t spots[CLUSTER_MAX_SPOTS];
    static int            n;
    static uint32_t       gen;
    int                   k = cluster_client_snapshot(spots, CLUSTER_MAX_SPOTS, &gen);

    if (k >= 0) n = k;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(spots[i].dx, call) && (cluster_spot_activity(&spots[i]) & CLUSTER_ACT_POTA)) return true;
    return false;
}

/* ---- the decoder ---- */

static void on_message(const char *text, int snr, float freq_hz, float time_sec, const slot_info_t *slot, void *ctx);
static void on_psd(const float *psd, uint16_t nfft, float sec, const slot_info_t *slot, void *ctx);
static void on_slot_end(const slot_info_t *slot, void *ctx);
static void on_tick(const slot_info_t *slot, bool new_slot, float sec, void *ctx);

static void decoder_start(void) {
    FTxQsoProcessor *p = ftx_qso_processor_init(params.callsign.x, station_qth(), qso_completed,
                                                param_i_get(cfg_ft8_max_repeats));

    ftx_qso_processor_set_is_pota(p, pota_spotted);
    ftx_qso_processor_set_auto(p, param_i_get(cfg_ft8_auto));     /* the setting, not qso.cpp's default (on) */
    qso_lock();     /* the panel's status reads it */
    qso        = p;
    decoder_on = true;
    qso_unlock();

    audio_worker_cb_t cb = {
        .on_message  = on_message,
        .on_psd      = on_psd,
        .on_slot_end = on_slot_end,
        .on_tick     = on_tick,
    };

    audio_worker = audio_worker_create(SAMPLE_RATE, cur_proto(), filter_low, filter_high, &cb);
    if (audio_worker) audio_worker_start(audio_worker);
}

static void decoder_stop(void) {
    /* Nothing goes out from here: a transmission under way stops, so the join does not wait for it */
    qso_lock();
    decoder_on    = false;
    sending       = false;
    tx_msg.msg[0] = '\0';
    tx_gen++;
    qso_unlock();
    if (audio_worker) {
        audio_worker_destroy(audio_worker);
        audio_worker = NULL;
    }
    radio_set_modem(false);
    qso_lock();     /* the decoder is gone, the panel's status may still look */
    if (qso) {
        ftx_qso_processor_delete(qso);
        qso = NULL;
    }
    tx_msg.msg[0] = '\0';  /* an answer its last decode queued */
    tx_gen++;
    qso_unlock();
    lv_finder_clear_cursor(finder);
}

/* ---- maps ---- */

/* QSO map: the world, the station and a path to each QSO of the FT8/FT4 log (js8_map.c) */
static void map_close(void) {
    if (!map_obj) return;
    lv_obj_del_async(map_obj);
    map_obj = NULL;
    give_list_the_mfk();
}

static void map_key_cb(lv_event_t *e) {
    if (*(uint32_t *)lv_event_get_param(e) == LV_KEY_ESC) map_close();
}

/* What the heard map keeps of a decode: enough to call it as a press on its row does */
typedef struct {
    ftx_msg_meta_t meta;
    bool           odd;
} map_rx_t;

_Static_assert(sizeof(map_rx_t) <= sizeof(((js8_map_station_t *)0)->data), "map_rx_t too big for the map");

/* MFK press on a station of the Live Map: back to the list and call it, as from its row */
static void map_pick(const js8_map_station_t *st) {
    const map_rx_t *rx = (const map_rx_t *)st->data;
    cell_data_t     cd = {.cell_type = CELL_RX_MSG, .odd = rx->odd, .meta = rx->meta};

    map_close();
    list_pressed(&cd);
}

/* Either map button closes the map that is open */
static void map_open(bool heard) {
    if (map_obj) {
        map_close();
        return;
    }
    if (heard)
        map_obj = js8_map_create(dialog.obj, WIDTH, WF_HEIGHT, station_qth(), BOARD_DATA "/ft_log.adi", js8_map_heard_fill,
                                 false, map_pick);
    else
        map_obj = js8_map_create(dialog.obj, WIDTH, WF_HEIGHT, station_qth(), BOARD_DATA "/ft_log.adi", NULL, true, NULL);
    if (!map_obj) {
        msg_update_text_fmt("Not enough memory for the map");
        return;
    }
    lv_obj_set_pos(map_obj, 13, 13);
    lv_group_remove_obj(table_view_obj());
    lv_obj_add_event_cb(map_obj, map_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, map_obj);
    lv_group_focus_obj(map_obj);
    lv_group_set_editing(keyboard_group, true);   /* the MFK steps through the stations */
}

/* ---- dial frequency ---- */

static bool load_band(int8_t dir) {
    lv_finder_set_width(finder, proto()->width_hz);
    if (!cfg_digital_load(dir, proto()->presets)) return false;
    msg_update_text_fmt("%s", cfg_digital_label_get());
    return true;
}

/* "14.091" from 14091000 Hz: MHz, 3 decimals at least */
static void format_mhz(int32_t hz, char *buf, size_t size) {
    snprintf(buf, size, "%d.%06d", (int)(hz / 1000000), (int)(hz % 1000000));
    char *end = buf + strlen(buf) - 1;
    while (*end == '0' && end > strchr(buf, '.') + 3) *end-- = '\0';
}

/* What the info rows call where we are: the preset's label, or "FT8 Custom 14.091" */
static const char *where_label(void) {
    static char buf[40];
    if (!params.ft8_custom_on.x) return cfg_digital_label_get();
    char mhz[16];
    format_mhz(params.ft8_custom_hz.x, mhz, sizeof(mhz));
    snprintf(buf, sizeof(buf), "%s Custom %s", mode_name(), mhz);
    return buf;
}

/* The band's preset, or the custom frequency (in the preset's mode) */
static void load_base(void) {
    load_band(0);
    if (!params.ft8_custom_on.x) return;
    if (params.ft8_custom_hz.x < CUSTOM_MIN_HZ || params.ft8_custom_hz.x > CUSTOM_MAX_HZ) {
        params_bool_set(&params.ft8_custom_on, false);
        return;
    }
    int32_t mode = cparam_i_get(cfg_cur_mode);
    cparam_i_set(cfg_fg_freq, params.ft8_custom_hz.x);
    if (cparam_i_get(cfg_cur_mode) != mode) cparam_i_set(cfg_cur_mode, mode); /* another band loads its own mode */
    msg_update_text_fmt("%s", where_label());
}

/* Empty list and waterfall, the MFK on the "Wait sync" row */
static void clear_view(void) {
    static uint32_t up = LV_KEY_UP;

    table_view_reset();
    lv_waterfall_clear_data(waterfall);
    lv_event_send(table_view_obj(), LV_EVENT_KEY, &up);
}

/* The decoder again from the start: after a change of dial frequency or mode. The queued
 * message goes with it, and TX CQ with a CQ: left on, it called nothing, and took two presses
 * to call again (#28). Returns whether a CQ ended */
static bool decoder_restart(void) {
    bool cq = subject_i_get(cq_enabled);

    subject_i_set(cq_enabled, false);
    decoder_stop();
    decoder_start();
    clear_view();
    return cq;
}

/* After any change of dial frequency. A CQ that ended says so after the band's label: the
 * band keys work from any button page, TX CQ may not be in view */
static void retuned(void) {
    qso_lock();
    hound_stop(&hound);
    qso_unlock();
    js8_map_heard_clear();
    if (decoder_restart()) msg_schedule_text_fmt("CQ off: press TX CQ to call on this frequency");
}

static void band_cb(lv_event_t *e) {
    if (!load_band(lv_event_get_code(e) == EVENT_BAND_UP ? 1 : -1)) return;
    if (params.ft8_custom_on.x) params_bool_set(&params.ft8_custom_on, false); /* back to the presets */
    retuned();
}

/* ---- TX frequency ---- */

static void set_tx_freq(int32_t hz) {
    hz = LV_MIN(hz, filter_high);
    hz = LV_MAX(hz, filter_low);
    params_uint16_set(&params.ft8_tx_freq, hz);
    lv_finder_set_value(finder, hz);
    lv_obj_invalidate(finder);
    freq_strip_set_offset(hz);
}

/* The TX frequency as far as the signal stays inside the filter */
static void set_tx_freq_in_filter(int32_t hz) {
    set_tx_freq(limit(hz, filter_low, filter_high - proto()->width_hz));
}

static void fade_exec_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa_layered(obj, v, 0);
}

static void fade_ready_cb(lv_anim_t *a) {
    (void)a;
    fading = false;
}

static void fade_to(lv_opa_t opa) {
    lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(table_view_obj(), 0), opa);
    lv_anim_start(&fade);
}

static void fade_back_cb(lv_timer_t *t) {
    (void)t;
    fade_to(LV_OPA_COVER);
    fade_back_timer = NULL;
}

/* The main knob: the TX frequency, the list faded to show the waterfall under it; the map zooms */
static void rotary_cb(int32_t diff) {
    /* Faster turns move further: 4-5 clicks within 30 ms move 5 Hz each, 6 or more
     * 10 Hz. The knob's detents arrive batched per GUI read (10 ms), so a single
     * diff almost never got past 3: count the clicks per 30 ms, as the JS8 app
     * does (from randal007's upstream ft8-knob-speed b2c3e1ac) */
    static uint32_t burst_start;
    static uint32_t burst_clicks;

    if (lv_tick_elaps(burst_start) >= 30) {
        burst_start  = lv_tick_get();
        burst_clicks = 0;
    }
    if (map_obj) {
        js8_map_zoom(diff);
        return;
    }
    burst_clicks += abs(diff);
    if (burst_clicks > 3) diff *= burst_clicks < 6 ? 5 : 10;
    set_tx_freq_in_filter((int32_t)params.ft8_tx_freq.x + diff);

    if (!fading) {
        fading = true;
        fade_to(LV_OPA_TRANSP);
    }
    if (fade_back_timer) {
        lv_timer_reset(fade_back_timer);
    } else {
        fade_back_timer = lv_timer_create(fade_back_cb, 1000, NULL);
        lv_timer_set_repeat_count(fade_back_timer, 1);
    }
}

/* ---- buttons ---- */

static const char *switch_label(char *buf, size_t size, const char *name, bool on) {
    snprintf(buf, size, "%s:\n%s", name, on ? "Enabled" : "Disabled");
    return buf;
}

static const char *show_label(void) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "Show:\n%s", param_i_get(cfg_ft8_show_all) ? "All" : "CQ");
    return buf;
}

static const char *mode_label(void) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "Mode:\n%s", mode_name());
    return buf;
}

static const char *tx_cq_label(void) {
    static char buf[32];
    return switch_label(buf, sizeof(buf), "TX CQ", subject_i_get(cq_enabled));
}

static const char *tx_call_label(void) {
    static char buf[32];
    return switch_label(buf, sizeof(buf), "TX Call", subject_i_get(tx_enabled));
}

static const char *hold_label(void) {
    static char buf[32];
    return switch_label(buf, sizeof(buf), "Hold Freq", param_i_get(cfg_ft8_hold_freq));
}

static const char *auto_label(void) {
    static char buf[32];
    return switch_label(buf, sizeof(buf), "Auto", param_i_get(cfg_ft8_auto));
}

static const char *foryou_label(void) {
    static char buf[32];
    return switch_label(buf, sizeof(buf), "For you", param_i_get(cfg_ft8_foryou));
}

static const char *freq_label(void) {
    static char buf[32];
    if (!params.ft8_custom_on.x) return "Freq:\nPreset";
    char mhz[16];
    format_mhz(params.ft8_custom_hz.x, mhz, sizeof(mhz));
    snprintf(buf, sizeof(buf), "Freq:\n%s", mhz);
    return buf;
}

static const char *hound_label(void) {
    return hound_on ? "Hound:\nOn" : "Hound:\nOff";
}

static const char *retries_label(void) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "Retries:\n%i", (int)param_i_get(cfg_ft8_max_repeats));
    return buf;
}

static void show_cb(button_data_t *btn) {
    if (kb_open) return;
    param_i_set(cfg_ft8_show_all, !param_i_get(cfg_ft8_show_all));
}

/* FT8 -> FT4 -> FT2 -> FT8 */
static void mode_cb(button_data_t *btn) {
    if (kb_open) return;

    int next = cur_proto() == FTX_PROTOCOL_FT8 ? FTX_PROTOCOL_FT4
             : cur_proto() == FTX_PROTOCOL_FT4 ? FTX_PROTOCOL_FT2 : FTX_PROTOCOL_FT8;

    param_i_set(cfg_ft8_protocol, next);
    freq_strip_set_app(ftx_protocol_name(next));
    decoder_restart();      /* a CQ ends with no note: TX CQ is next to Mode */
    if (params.ft8_custom_on.x) params_bool_set(&params.ft8_custom_on, false); /* FT4/FT2 elsewhere */
    if (next != FTX_PROTOCOL_FT8) {
        qso_lock();
        hound_on = false;
        qso_unlock();
    }
    load_band(0);
}

/* The slot to send in: the next one, or this one when it began a moment ago */
static bool first_tx_slot_odd(void) {
    struct timespec now;
    float           slot = ftx_slot_time(cur_proto());

    clock_gettime(CLOCK_REALTIME, &now);

    float sec  = (now.tv_sec % 60) + now.tv_nsec / 1.0e9f;
    bool  odd  = (int)(sec / slot) % 2;
    bool  next = !odd;

    return fmodf(sec, slot) < tx_start_limit() ? !next : next;
}

static void tx_cq_cb(button_data_t *btn) {
    if (kb_open) return;
    if (subject_i_get(cq_enabled)) {
        subject_i_set(cq_enabled, false);
        qso_lock();
        tx_msg.msg[0] = '\0';
        tx_gen++;
        sending = false;    /* the CQ under way too */
        qso_unlock();
        return;
    }
    if (hound_on) {
        msg_schedule_text_fmt("No CQ in Hound mode");
        return;
    }
    if (!params.callsign.x[0]) {
        msg_schedule_text_fmt("Call sign required");
        return;
    }
    subject_i_set(cq_enabled, true);
    subject_i_set(tx_enabled, true);
    qso_lock();
    cq_make_message(params.callsign.x, station_qth(), params.ft8_cq_modifier.x, tx_msg.msg);
    tx_odd = first_tx_slot_odd();
    /* "CQ_DX ON6URE JO21" goes out as "CQ DX ..." */
    msg_schedule_text_fmt("Next TX: %s%s", tx_msg.msg[2] == '_' ? "CQ " : "", tx_msg.msg + (tx_msg.msg[2] == '_' ? 3 : 0));
    tx_msg.repeats = param_i_get(cfg_ft8_max_repeats);
    ftx_qso_processor_reset(qso);
    tx_gen++;
    qso_unlock();
    lv_finder_clear_cursor(finder);
}

/* TX Call off: no transmission starts, the one under way stops (not with qso_mux held) */
static void tx_call_off(void) {
    qso_lock();
    subject_i_set(tx_enabled, false);
    sending = false;
    qso_unlock();
}

static void tx_call_cb(button_data_t *btn) {
    if (kb_open) return;
    if (subject_i_get(tx_enabled)) {
        tx_call_off();
        msg_schedule_text_fmt("TX off: receive only until TX Call, CQ or a call is pressed");
    } else if (!params.callsign.x[0]) {
        msg_schedule_text_fmt("Call sign required");
    } else {
        subject_i_set(tx_enabled, true);
    }
}

static void hold_cb(button_data_t *btn) {
    if (kb_open) return;
    param_i_set(cfg_ft8_hold_freq, !param_i_get(cfg_ft8_hold_freq));
}

static void auto_cb(button_data_t *btn) {
    if (kb_open) return;
    bool on = !param_i_get(cfg_ft8_auto);
    param_i_set(cfg_ft8_auto, on);
    qso_lock();
    ftx_qso_processor_set_auto(qso, on);
    qso_unlock();
}

/* One step: a QSO the decoder completes meanwhile is saved once, by it or here */
static void force_save_cb(button_data_t *btn) {
    qso_lock();
    bool saved = ftx_qso_processor_can_save_qso(qso) && ftx_qso_processor_force_save_qso(qso);
    qso_unlock();
    if (!saved) msg_schedule_text_fmt("Can't save incomplete QSO");
}

static void cq_modifier_cb(button_data_t *btn) {
    if (kb_open) return;
    lv_group_remove_obj(table_view_obj());
    textarea_window_open(kb_ok_cb, kb_cancel_cb);

    lv_obj_t *text = textarea_window_text();

    lv_textarea_set_accepted_chars(text, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    if (params.ft8_cq_modifier.x[0])
        textarea_window_set(params.ft8_cq_modifier.x);
    else
        lv_textarea_set_placeholder_text(text, " CQ modifier");
    kb_open = true;
}

/* Time Sync, pressed as a slot starts: the system clock to the nearest slot start, for a clock
 * off by a few seconds with nothing to set it. A clock that ntpd keeps (WiFi, or the GPS through
 * it) is right already, and moved to where the press fell it would be off by up to half a slot:
 * it is left alone. The step is taken from the time to the nanosecond (FT4's and FT2's slots
 * start on fractions of a second, whole seconds missed them), and only a clock that moved goes
 * into the RTC */
#define SYNC_GPS_AGE_S  10      /* a GPS fix this recent: gpsd feeds ntpd */
#define SYNC_ON_SLOT_NS 50000000

static void time_sync_cb(button_data_t *btn) {
    struct timespec now;
    double          lat, lon;
    int             err_ms, age;
    int64_t         off, t;

    if (ftx_clock_synced(&err_ms)) {
        msg_update_text_fmt("Clock in sync (NTP, within %d ms): no Time Sync needed", err_ms);
        return;
    }
    if (gps_last_fix(&lat, &lon, &age) && age <= SYNC_GPS_AGE_S) {
        msg_update_text_fmt("The GPS keeps the clock: no Time Sync needed");
        return;
    }
    clock_gettime(CLOCK_REALTIME, &now);
    off = ftx_slot_offset_ns(now.tv_sec, now.tv_nsec, ftx_slot_time(cur_proto()));
    if (llabs(off) < SYNC_ON_SLOT_NS) {
        msg_update_text_fmt("Clock on the slot already (%+.2f s)", off / 1e9);
        return;
    }
    clock_gettime(CLOCK_REALTIME, &now);    /* the step on the time as it is now: nothing lost */
    t = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec - off;

    struct timespec ts = {.tv_sec = t / 1000000000LL, .tv_nsec = t % 1000000000LL};

    if (clock_settime(CLOCK_REALTIME, &ts)) {
        const char *why = strerror(errno);

        LV_LOG_ERROR("Can't set system time: %s\n", why);
        msg_update_text_fmt("Time Sync: the clock can't be set (%s)", why);
        return;
    }
    /* into the RTC too, in UTC as the kernel reads it at boot: kept across a power-off */
    sh_run("hwclock -w -u -f /dev/rtc1", NULL, NULL);
    msg_update_text_fmt("Clock moved %+.2f s to the slot start", -off / 1e9);
}

static void map_cb(button_data_t *btn) {
    map_open(false);
}

/* The stations decoded in the last 15 minutes, from their grid */
static void heard_map_cb(button_data_t *btn) {
    map_open(true);
}

/* Freq: the mode's preset frequencies, one for each band, from the digital_modes table */
static cfg_digital_preset_t presets[48];

static void freq_close(void) {
    if (!freq_list) return;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(freq_list); i++) lv_group_remove_obj(lv_obj_get_child(freq_list, i));
    lv_obj_del_async(freq_list);    /* called from one of its buttons */
    freq_list = NULL;
    give_list_the_mfk();
}

static void preset_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    freq_close();
    if (i < 0) return;   /* Close */
    params_bool_set(&params.ft8_custom_on, false);
    cparam_i_set(cfg_fg_freq, presets[i].freq);
    load_band(0);
    retuned();
    buttons_refresh(&button_freq);
}

static void preset_key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (key == LV_KEY_ESC) freq_close();
    else if (key == LV_KEY_LEFT || key == LV_KEY_UP) lv_group_focus_prev(keyboard_group);
    else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) lv_group_focus_next(keyboard_group);
}

static void preset_focused_cb(lv_event_t *e) {
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_OFF);
}

static lv_obj_t *preset_add(const char *label, int i) {
    lv_obj_t *b = lv_list_add_btn(freq_list, NULL, label);
    lv_obj_set_style_bg_color(b, TC(row), 0);
    lv_obj_set_style_bg_color(b, TC(focus_bg), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(b, TC(text), 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(b, preset_focused_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(b, preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(b, preset_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, b);
    return b;
}

/* Freq or its hold: not while sending, not over a map */
static bool freq_allowed(void) {
    if (sending) {
        msg_schedule_text_fmt("Not while sending");
        return false;
    }
    return !map_obj;
}

static void freq_cb(button_data_t *btn) {
    if (kb_open) return;
    if (freq_list) {    /* Freq again closes it */
        freq_close();
        return;
    }
    if (!freq_allowed()) return;

    int n = cfg_digital_list(proto()->presets, presets, ARRAY_SIZE(presets));

    lv_group_remove_obj(table_view_obj());
    freq_list = lv_list_create(dialog.obj);
    lv_obj_set_size(freq_list, 400, WF_HEIGHT - 10);
    lv_obj_align(freq_list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(freq_list, &sony_24, 0);
    lv_obj_set_style_bg_color(freq_list, TC(list), 0);
    lv_obj_set_style_border_color(freq_list, TC(text), 0);
    char line[64];
    snprintf(line, sizeof(line), "%s frequencies - hold Freq to type", mode_name());
    lv_obj_set_style_text_font(lv_list_add_text(freq_list, line), &sony_22, 0);

    lv_obj_t *cur = NULL;
    for (int i = 0; i < n; i++) {
        char mhz[16];
        format_mhz(presets[i].freq, mhz, sizeof(mhz));
        snprintf(line, sizeof(line), "%s  %s", mhz, presets[i].label);
        lv_obj_t *b = preset_add(line, i);
        if (!params.ft8_custom_on.x && presets[i].freq == cfg_digital_freq_get()) {
            lv_obj_set_style_text_color(b, TC(ok), 0);   /* the one in use */
            cur = b;
        }
    }
    lv_obj_t *close = preset_add("Close", -1);
    lv_obj_set_style_text_color(close, TC(action), 0);
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(cur ? cur : close);
}

/* Hold Freq: the dial frequency from the keyboard */
static void freq_hold_cb(button_data_t *btn) {
    if (kb_open) return;
    freq_close();
    if (!freq_allowed()) return;
    lv_group_remove_obj(table_view_obj());
    textarea_window_open(kb_ok_cb, kb_cancel_cb);
    lv_obj_t *text = textarea_window_text();
    lv_textarea_set_accepted_chars(text, "0123456789.");
    lv_textarea_set_placeholder_text(text, " 14091 or 14.091, empty: presets");
    if (params.ft8_custom_hz.x) {
        char mhz[16];
        format_mhz(params.ft8_custom_hz.x, mhz, sizeof(mhz));
        textarea_window_set(mhz);
    }
    kb_freq = true;
    kb_open = true;
    msg_schedule_text_fmt("Dial frequency (14091 or 14.091), empty for the presets");
}

/* DXpedition Hound: on or off, a QSO or a CQ in progress stops */
static void hound_cb(button_data_t *btn) {
    if (kb_open) return;
    if (!hound_on && cur_proto() != FTX_PROTOCOL_FT8) {
        msg_schedule_text_fmt("Hound mode is for FT8 only");
        return;
    }
    subject_i_set(cq_enabled, false);
    qso_lock();
    sending  = false;
    hound_on = !hound_on;
    tx_msg.msg[0] = '\0';
    memset(&hound, 0, sizeof(hound));
    ftx_qso_processor_reset(qso);
    tx_gen++;
    qso_unlock();
    lv_finder_clear_cursor(finder);
    buttons_refresh(&button_hound);
    if (hound_on) {
        add_info("Hound on: select the Fox's CQ and press, calls go above %d Hz", HOUND_MIN_TX_HZ);
        msg_schedule_text_fmt("Hound on: select the Fox's CQ and press");
    } else {
        add_info("Hound off");
    }
}

/* How many times a message goes out before the QSO is given up: the next step, round */
static void retries_cb(button_data_t *btn) {
    static const int steps[] = {3, 4, 5, 6, 8, 10, 12, 15};
    int              cur = param_i_get(cfg_ft8_max_repeats), next = steps[0];

    if (kb_open) return;
    for (size_t i = 0; i < ARRAY_SIZE(steps); i++)
        if (steps[i] > cur) {
            next = steps[i];
            break;
        }
    param_i_set(cfg_ft8_max_repeats, next);
    qso_lock();
    ftx_qso_processor_set_max_repeats(qso, next);
    qso_unlock();
}

static void foryou_cb(button_data_t *btn) {
    param_i_set(cfg_ft8_foryou, !param_i_get(cfg_ft8_foryou));
    table_view_pin_clear();
}

static buttons_page_t page_1, page_2, page_3, page_4, page_5;

/* The labels show the state now; a press changes it */
static button_data_t button_page_1  = {.type = BTN_TEXT, .label = "(FT8 1:5)", .press = button_next_page_cb, .next = &page_2};
static button_data_t button_show    = {.type = BTN_TEXT_FN, .label_fn = show_label, .press = show_cb};
static button_data_t button_mode    = {.type = BTN_TEXT_FN, .label_fn = mode_label, .press = mode_cb};
static button_data_t button_tx_cq   = {.type = BTN_TEXT_FN, .label_fn = tx_cq_label, .press = tx_cq_cb};
static button_data_t button_tx_call = {.type = BTN_TEXT_FN, .label_fn = tx_call_label, .press = tx_call_cb};

static button_data_t button_page_2  = {.type = BTN_TEXT, .label = "(FT8 2:5)", .press = button_next_page_cb, .next = &page_3};
static button_data_t button_hold    = {.type = BTN_TEXT_FN, .label_fn = hold_label, .press = hold_cb};
static button_data_t button_auto    = {.type = BTN_TEXT_FN, .label_fn = auto_label, .press = auto_cb};
static button_data_t button_force   = {.type = BTN_TEXT, .label = "Force QSO\nsave", .press = force_save_cb};

static button_data_t button_page_3  = {.type = BTN_TEXT, .label = "(FT8 3:5)", .press = button_next_page_cb, .next = &page_4};
static button_data_t button_cq_mod  = {.type = BTN_TEXT, .label = "CQ\nModifier", .press = cq_modifier_cb};
static button_data_t button_sync    = {.type = BTN_TEXT, .label = "Time\nSync", .press = time_sync_cb};
static button_data_t button_log_map = {.type = BTN_TEXT, .label = "Log\nMap", .press = map_cb};
static button_data_t button_live_map = {.type = BTN_TEXT, .label = "Live\nMap", .press = heard_map_cb};

static button_data_t button_page_4  = {.type = BTN_TEXT, .label = "(FT8 4:5)", .press = button_next_page_cb, .next = &page_5};
static button_data_t button_freq    = {.type = BTN_TEXT_FN, .label_fn = freq_label, .press = freq_cb, .hold = freq_hold_cb};
static button_data_t button_hound   = {.type = BTN_TEXT_FN, .label_fn = hound_label, .press = hound_cb};
static button_data_t button_retries = {.type = BTN_TEXT_FN, .label_fn = retries_label, .press = retries_cb};
static button_data_t button_foryou  = {.type = BTN_TEXT_FN, .label_fn = foryou_label, .press = foryou_cb};

/* TUNE for an external tuner: a tone at the TX offset, where the app transmits (ext_tune.h) */
static button_data_t button_page_5  = {.type = BTN_TEXT, .label = "(FT8 5:5)", .press = button_next_page_cb, .next = &page_1};

static buttons_page_t page_1 = {{&button_page_1, &button_show, &button_mode, &button_tx_cq, &button_tx_call}};
static buttons_page_t page_2 = {{&button_page_2, &button_hold, &button_auto, &button_force, &btn_dig_pwr}};
static buttons_page_t page_3 = {{&button_page_3, &button_cq_mod, &button_sync, &button_log_map, &button_live_map}};
static buttons_page_t page_4 = {{&button_page_4, &button_freq, &button_hound, &button_retries, &button_foryou}};
static buttons_page_t page_5 = {{&button_page_5, &btn_ext_tune}};

/* ---- keyboard window ---- */

static void kb_close(void) {
    textarea_window_close();
    lv_group_add_obj(keyboard_group, table_view_obj());
    lv_group_set_editing(keyboard_group, true);
    kb_open = false;
    kb_freq = false;
}

static bool kb_cancel_cb(void) {
    kb_close();
    return true;
}

/* The dial frequency typed: kHz or MHz, empty for the presets */
static bool freq_typed(void) {
    const char *text = textarea_window_get();

    if (text[0]) {
        char   *end;
        double  v  = strtod(text, &end);
        int32_t hz = (int32_t)(v < 100 ? v * 1e6 + 0.5 : v * 1e3 + 0.5);
        if (end == text || *end || hz < CUSTOM_MIN_HZ || hz > CUSTOM_MAX_HZ) {
            msg_schedule_text_fmt("Type 1800 - 54000 kHz, e.g. 14091");
            return false;
        }
        params_int32_set(&params.ft8_custom_hz, hz);
    }
    params_bool_set(&params.ft8_custom_on, text[0] != '\0');
    kb_close();
    load_base();
    retuned();
    buttons_refresh(&button_freq);
    return true;
}

static bool kb_ok_cb(void) {
    if (kb_freq) return freq_typed();

    char *mod = (char *)textarea_window_get();

    if (mod[0] && !is_cq_modifier(mod)) {
        msg_schedule_text_fmt("Unsupported CQ modifier");
        return false;
    }
    params_str_set(&params.ft8_cq_modifier, mod);
    kb_close();
    return true;
}

/* ---- the list ---- */

/* A row of the list (or a station of the Live Map, or a remote reply): answer it */
static void list_pressed(const cell_data_t *cd) {
    if (sending) {
        tx_call_off();
        return;
    }
    if (!cd || cd->cell_type == CELL_TX_MSG || cd->cell_type == CELL_RX_INFO || cd->cell_type == CELL_START_QSO) {
        msg_schedule_text_fmt("What should I do about it?");
        return;
    }

    if (hound_on) {
        if (!cd->meta.call_de[0] || strlen(params.callsign.x) == 0) {
            msg_schedule_text_fmt(cd->meta.call_de[0] ? "Call sign required" : "Select the Fox's CQ");
            return;
        }
        qso_lock();
        hound_call(&hound, params.callsign.x, station_qth(), cd->meta.call_de, cd->meta.grid,
                   time(NULL), tx_msg.msg);
        tx_msg.repeats = -1;    /* until the Fox answers or the 2 minutes are over */
        tx_odd         = true;  /* the Fox sends first, the Hounds second */
        tx_gen++;
        add_info("Hound: calling %s above %d Hz", hound.fox, HOUND_MIN_TX_HZ);
        msg_schedule_text_fmt("Next TX: %s", tx_msg.msg);
        qso_unlock();
        lv_finder_set_cursor(finder, cd->meta.freq_hz);
        subject_i_set(tx_enabled, true);
        return;
    }

    char was[sizeof(tx_msg.msg)], next[sizeof(tx_msg.msg)];

    qso_lock();
    snprintf(was, sizeof(was), "%s", tx_msg.msg);
    ftx_qso_processor_start_qso(qso, (ftx_msg_meta_t *)&cd->meta, &tx_msg);
    snprintf(next, sizeof(next), "%s", tx_msg.msg);
    if (next[0]) tx_odd = !cd->odd;
    tx_gen++;
    qso_unlock();
    if (!next[0]) {
        msg_schedule_text_fmt("Invalid message");
        tx_call_off();
        return;
    }
    lv_finder_set_cursor(finder, cd->meta.freq_hz);
    if (!param_i_get(cfg_ft8_hold_freq)) set_tx_freq(cd->meta.freq_hz);
    subject_i_set(tx_enabled, true);
    /* The answer took the CQ's place: CQ off, as when the sequence answers (#28). Told by a change
     * of the message, as add_rx_text does, not by a "CQ" at its start: a call can begin with CQ
     * (CQ7X). A 73 addressed to me leaves the message as it was, and the CQ goes on */
    if (strcmp(was, next)) subject_i_set(cq_enabled, false);

    cell_data_t row = {.cell_type = CELL_START_QSO};

    snprintf(row.text, sizeof(row.text), "Start QSO with %s", cd->meta.call_de);
    row_post(&row);
    msg_schedule_text_fmt("Next TX: %s", next);
}

static void list_close(void) {
    dialog_destruct(&dialog);
}

static void list_vol(int32_t dir) {
    radio_change_vol(dir);
}

/* An info row: the slot rows, Hound's notes */
static void add_info(const char *fmt, ...) {
    va_list     ap;
    cell_data_t row = {.cell_type = CELL_RX_INFO};

    va_start(ap, fmt);
    vsnprintf(row.text, sizeof(row.text), fmt, ap);
    va_end(ap);
    row_post(&row);
}

/* Our transmission as a row, "12:00:15 DL1ABC ON6URE JO21" */
static void add_tx_row(const char *msg) {
    cell_data_t     row = {.cell_type = CELL_TX_MSG};
    char            text[sizeof(row.text)];
    struct timespec now;

    snprintf(text, sizeof(text), "%s", msg);
    if (!strncmp(text, "CQ_", 3)) text[2] = ' ';
    clock_gettime(CLOCK_REALTIME, &now);
    strftime(row.text, 10, "%H:%M:%S ", gmtime(&now.tv_sec));
    snprintf(row.text + 9, sizeof(row.text) - 9, "%s", text);
    row_post(&row);
}

/* Hound: the Fox's report gets R+report, its RR73 logs the QSO. Under qso_mux: the GUI's part
 * goes into j, posted after */
static void hound_rx_text(const char *text, int snr, float freq_hz, bool odd, ui_job_t *j) {
    char          tx[sizeof(tx_msg.msg)];
    hound_stage_t was = hound.stage;
    hound_event_t ev  = hound_rx(&hound, text, snr, (int)(freq_hz + 0.5f), odd, time(NULL), tx);

    if (ev == HOUND_REPLY) {
        strcpy(tx_msg.msg, tx);
        tx_msg.repeats = -1;
        tx_odd         = true;
        j->cursor      = true;
        j->cursor_hz   = freq_hz;
        add_info("Hound: %s sent %+03d, answering R%+03d at %d Hz", hound.fox, hound.rcvd, hound.sent, hound.fox_freq);
        msg_schedule_text_fmt("Next TX: %s", tx_msg.msg);
    } else if (ev == HOUND_LOGGED) {
        tx_msg.msg[0] = '\0';
        qso_completed(hound.fox, hound.fox_grid, hound.rcvd, hound.sent);
        add_info("Hound: QSO with %s logged", hound.fox);
    } else if (was != HOUND_IDLE && hound.stage == HOUND_IDLE) {
        tx_msg.msg[0] = '\0';
        add_info("Hound: %s sent RR73 but its report was missed, not logged", hound.fox);
    }
}

/* For the RX rows, under qso_mux */
static const char *hound_state(void) {
    static char buf[40];
    switch (hound.stage) {
        case HOUND_CALLING: snprintf(buf, sizeof(buf), "calling %s", hound.fox); break;
        case HOUND_REPORT:  snprintf(buf, sizeof(buf), "R%+03d to %s", hound.sent, hound.fox); break;
        default:            snprintf(buf, sizeof(buf), "select the Fox"); break;
    }
    return buf;
}

/* The sender's country and flag, from the call (cty.dat), as JTDX (from SP2KMO's upstream PR #282) */
static void set_country(cell_data_t *row, const char *call) {
    const char *name, *prefix;

    row->flag = -1;
    if (!dxcc_lookup(call, &name, &prefix)) return;
    snprintf(row->country, sizeof(row->country), "%s", name);
    snprintf(row->cty_prefix, sizeof(row->cty_prefix), "%s", prefix);
    for (size_t i = 0; i < ARRAY_SIZE(flag_of); i++) {
        if (!strcmp(flag_of[i].prefix, prefix)) {
            row->flag = (int16_t)flag_of[i].flag;
            break;
        }
    }
}

/* A decode: through the QSO sequence (or Hound's), to the remote, the map, PSK Reporter, then
 * its row unless Show CQ hides it */
static void add_rx_text(int16_t snr, const char *text, bool odd, float freq_hz, float time_sec) {
    ftx_msg_meta_t meta = {.freq_hz = freq_hz, .time_sec = time_sec};  /* call_de stays "" for unknown messages */
    ftx_tx_msg_t   ignored;
    char           was[sizeof(tx_msg.msg)];
    ui_job_t       j = {0};

    /* One step of the sequence: a QSO it completes is saved after it, the GUI's part posted after */
    qso_lock();
    snprintf(was, sizeof(was), "%s", tx_msg.msg);
    int done = done_n;
    ftx_qso_processor_add_rx_text(qso, text, snr, &meta, hound_on ? &ignored : &tx_msg);

    if (hound_on) {
        /* The processor follows the Fox too, its answers unsent, and may complete the QSO: Hound's
         * RR73 alone logs it */
        done_n = done;
        hound_rx_text(text, snr, freq_hz, odd, &j);
    } else if (tx_msg.msg[0] && strcmp(was, tx_msg.msg)) {    /* the sequence answers it */
        j.cursor    = true;
        j.cursor_hz = meta.freq_hz;
        j.cq_off    = true;
        if (!param_i_get(cfg_ft8_hold_freq)) ui_job_tx_freq(&j, (int32_t)freq_hz);
        tx_odd = !odd;
        msg_schedule_text_fmt("Next TX: %s", tx_msg.msg);
    }
    j.tx_gen = tx_gen;
    qso_unlock();
    if (j.cursor || j.txfreq || j.cq_off) ui_post(&j);

    bool parsed = meta.type != FXT_MSG_TYPE_OTHER && meta.call_de[0];

    /* The heard map and PSK Reporter: the sender of any message we can parse */
    if (parsed) {
        map_rx_t rx = {meta, odd};
        js8_map_heard(meta.call_de, meta.grid, snr, meta.to_me, &rx, sizeof(rx));
        if (param_i_get(cfg_pskreporter)) {
            pskr_spot(params.callsign.x, station_qth(), VERSION, meta.call_de, meta.grid,
                      (uint32_t)cparam_i_get(cfg_fg_freq) + (uint32_t)(freq_hz + 0.5f), snr, mode_name());
        }
    }

    cell_data_t row = {.odd = odd, .meta = meta};

    if (meta.to_me)
        row.cell_type = CELL_RX_TO_ME;
    else if (meta.type == FTX_MSG_TYPE_CQ)
        row.cell_type = CELL_RX_CQ;
    else if (param_i_get(cfg_ft8_show_all))
        row.cell_type = CELL_RX_MSG;
    else
        return;

    if (meta.type == FTX_MSG_TYPE_CQ)
        row.worked_type = qso_log_search_worked(meta.call_de, proto()->log_mode,
                                                qso_log_freq_to_band(cparam_i_get(cfg_fg_freq)));
    row.pota = meta.call_de[0] && (!strncmp(text, "CQ POTA ", 8) || pota_spotted(meta.call_de));
    snprintf(row.text, sizeof(row.text), "%s %s", slot_time, text);
    if (parsed) set_country(&row, meta.call_de);
    else row.flag = -1;
    if (station_qth()[0] && meta.grid[0]) {
        double lat, lon;
        qth_str_to_pos(meta.grid, &lat, &lon);
        row.dist = qth_pos_dist(lat, lon, my_lat, my_lon);
    }
    row_post(&row);
}

/* ---- audio_worker callbacks (the decoder thread) ---- */

static void on_message(const char *text, int snr, float freq_hz, float time_sec, const slot_info_t *slot, void *ctx) {
    /* The loggers get it before anything else here (its socket never blocks) */
    ft8_udp_send_decode(true, cur_proto() == FTX_PROTOCOL_FT8 ? "~" : "+", text, (int32_t)snr, time_sec,
                        (uint32_t)(freq_hz + 0.5f));
    add_rx_text((int16_t)snr, text, slot->odd, freq_hz, time_sec);
}

typedef struct {
    float   *db;
    size_t   n;
    uint32_t win_gen;
} wf_row_t;

/* Into the waterfall of the window it was made for, if that is still open; freed either way */
static void wf_row_add(void *data) {
    wf_row_t *row = data;

    if (row->win_gen == win_gen) lv_waterfall_add_data(waterfall, row->db, row->n);
    free(row->db);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* A waterfall row: the filter's part of the spectrum, in dB over the noise floor */
static void on_psd(const float *psd, uint16_t nfft, float sec, const slot_info_t *slot, void *ctx) {
    if (!psd || !nfft) return;

    uint32_t lo = nfft / 2u + (uint32_t)nfft * filter_low / SAMPLE_RATE;
    uint32_t hi = LV_MIN(nfft / 2u + (uint32_t)nfft * filter_high / SAMPLE_RATE, nfft);

    if (lo >= hi) return;

    wf_row_t row = {.n = hi - lo, .win_gen = win_gen};

    row.db = calloc(row.n, sizeof(float));
    if (!row.db) return;
    memcpy(row.db, &psd[lo], row.n * sizeof(float));

    /* Relative to the noise floor (the 30th percentile of the row, smoothed over ~2 s), as
     * JS8: the fixed -27..0 dB left it near black at the radio's audio level */
    static float floor_db;
    static bool  floor_set;
    float       *sorted = malloc(row.n * sizeof(float));
    if (sorted) {
        memcpy(sorted, row.db, row.n * sizeof(float));
        qsort(sorted, row.n, sizeof(float), cmp_float);

        float p30 = sorted[row.n * 3 / 10];

        floor_db  = floor_set ? floor_db + 0.05f * (p30 - floor_db) : p30;
        floor_set = true;
        free(sorted);
    }
    for (size_t i = 0; i < row.n; i++) row.db[i] -= floor_db;
    if (!scheduler_put(wf_row_add, &row, sizeof(row))) free(row.db);    /* a full queue: this row is lost */
}

static void on_slot_end(const slot_info_t *slot, void *ctx) {
    qso_lock();
    if (qso) ftx_qso_processor_start_new_slot(qso);
    qso_unlock();
}

static bool tx_stopped_cb(void *ctx) {
    return !sending;
}

/* Our transmission, when one is due in this slot; returns whether it went out. The message is
 * taken under qso_mux and sent without it: the GUI may stop or change it meanwhile */
static bool transmit(const slot_info_t *slot, float sec) {
    ftx_tx_msg_t out;
    ui_job_t     j = {0};
    uint32_t     gen;

    qso_lock();
    if (!decoder_on || sec >= tx_start_limit() || !tx_msg.msg[0] || tx_odd != slot->odd ||
        !subject_i_get(tx_enabled)) {
        qso_unlock();
        return false;
    }
    if (hound_on) {
        if (hound_timed_out(&hound, time(NULL))) {
            tx_msg.msg[0] = '\0';
            add_info("Hound: no answer from %s, TX stopped", hound.fox);
            qso_unlock();
            return true;
        }
        int hz = hound_tx_freq(&hound, params.ft8_tx_freq.x, (unsigned)rand());
        if (hz != params.ft8_tx_freq.x) ui_job_tx_freq(&j, hz);
    }
    out     = tx_msg;
    gen     = tx_gen;
    sending = true;     /* a stop from now on clears it */
    qso_unlock();
    if (j.txfreq) {
        /* Hound's offset to the GUI, whose queue may keep it waiting: stopped or changed meanwhile,
         * this message does not go */
        ui_post(&j);
        qso_lock();
        bool go = sending && tx_gen == gen;
        sending = go;
        qso_unlock();
        if (!go) return false;
    }

    add_tx_row(out.msg);
    tx_worker_run(out.msg, AUDIO_PLAY_RATE, base_gain_offset, tx_stopped_cb, NULL);

    /* It counts for the message that went out, not for one the operator chose meanwhile */
    qso_lock();
    sending = false;
    j = (ui_job_t){.tx_gen = tx_gen};
    if (tx_gen == gen) {
        if (tx_msg.repeats > 0) tx_msg.repeats--;
        if (tx_msg.repeats == 0) {      /* given up: a CQ ends with it */
            j.cq_off      = !strncmp(tx_msg.msg, "CQ", 2);
            tx_msg.msg[0] = '\0';
        }
    }
    qso_unlock();
    if (j.cq_off) ui_post(&j);
    return true;
}

static void on_tick(const slot_info_t *slot, bool new_slot, float sec, void *ctx) {
    qso_lock();
    bool tx_pending = tx_msg.msg[0] != '\0';
    qso_unlock();

    if (transmit(slot, sec) || !new_slot) return;

    static unsigned ft2_slots;
    struct timespec now;

    sending = false;
    clock_gettime(CLOCK_REALTIME, &now);

    struct tm *utc = gmtime(&now.tv_sec);

    snprintf(slot_time, sizeof(slot_time), "%02i:%02i:%02i", utc->tm_hour, utc->tm_min, utc->tm_sec);

    /* FT2: an RX line every 3.75 s would fill the table; one every 4th slot (15 s), as FT8 */
    if (tx_pending && subject_i_get(tx_enabled)) return;
    if (is_ft2() && ft2_slots++ % 4) return;
    qso_lock();
    if (hound_on)
        add_info("RX %s %s  Hound: %s", where_label(), slot_time, hound_state());
    else
        add_info("RX %s %s", where_label(), slot_time);
    qso_unlock();
}

/* ---- the window ---- */

static void audio_cb(unsigned int n, float *samples) {
    if (!sending) audio_worker_feed(audio_worker, n, samples);
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC) dialog_destruct(&dialog);
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT) radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT) radio_change_vol(1);
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    pthread_mutex_lock(&save_mux);
    last_logged[0] = '\0';      /* the timeline's "Logged" is of this session */
    pthread_mutex_unlock(&save_mux);

    /* The window covers the main screen's spectrum and waterfall: no DSP for them */
    dsp_set_waterfall_enabled(false);
    dsp_set_spectrum_enabled(false);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    button_show.subj    = (Subject *)cfg_ft8_show_all;
    button_mode.subj    = (Subject *)cfg_ft8_protocol;
    button_hold.subj    = (Subject *)cfg_ft8_hold_freq;
    button_auto.subj    = (Subject *)cfg_ft8_auto;
    button_foryou.subj  = (Subject *)cfg_ft8_foryou;
    button_retries.subj = (Subject *)cfg_ft8_max_repeats;

    /* TX CQ starts off each time, TX Call as it was left */
    if (cq_enabled) {
        subject_i_set(cq_enabled, false);
    } else {
        cq_enabled        = subject_i_create(false);
        button_tx_cq.subj = (Subject *)cq_enabled;
    }
    if (!tx_enabled) {
        tx_enabled          = subject_i_create(true);
        button_tx_call.subj = (Subject *)tx_enabled;
    }
    buttons_load_page(&page_1);

    waterfall = lv_waterfall_create(dialog.obj);
    lv_obj_add_style(waterfall, &waterfall_style, 0);
    lv_obj_clear_flag(waterfall, LV_OBJ_FLAG_SCROLLABLE);
    lv_waterfall_set_palette(waterfall, (lv_color_t *)wf_palette, 256);
    lv_waterfall_set_size(waterfall, WIDTH, WF_HEIGHT);
    lv_waterfall_set_min(waterfall, 0);    /* above the noise floor (on_psd), as JS8 */
    lv_waterfall_set_max(waterfall, 30);
    lv_obj_set_pos(waterfall, 13, 13);

    finder = lv_finder_create(waterfall);
    lv_finder_set_width(finder, protos[FTX_PROTOCOL_FT8].width_hz);
    lv_finder_set_value(finder, params.ft8_tx_freq.x);
    freq_strip_set_offset(params.ft8_tx_freq.x);
    lv_obj_set_size(finder, WIDTH, WF_HEIGHT);
    lv_obj_set_pos(finder, 0, 0);
    lv_obj_set_style_radius(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(finder, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(finder, bg_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(finder, LV_OPA_50, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(finder, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(finder, TC(text), LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    /* See-through: the waterfall's top rows clear above it, the rest visible under it */
    table_view_build(dialog.obj, 13, 13 + WF_VISIBLE, WIDTH, WF_HEIGHT - WF_VISIBLE);
    table_view_set_press_cb(list_pressed);
    table_view_set_actions(&(table_view_actions_t){.on_close = list_close, .on_vol_change = list_vol});

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, table_view_obj());
    lv_anim_set_time(&fade, 250);
    lv_anim_set_exec_cb(&fade, fade_exec_cb);
    lv_anim_set_ready_cb(&fade, fade_ready_cb);

    lv_group_add_obj(keyboard_group, table_view_obj());
    lv_group_set_editing(keyboard_group, true);

    load_base();
    hound_on = false;
    memset(&hound, 0, sizeof(hound));

    filter_low  = cparam_i_get(cfg_cur_filter_low);
    filter_high = cparam_i_get(cfg_cur_filter_high);
    lv_finder_set_range(finder, filter_low, filter_high);
    qth_str_to_pos(station_qth(), &my_lat, &my_lon);

    main_screen_lock_ab(true);
    main_screen_lock_mode(true);
    main_screen_lock_freq(true);
    main_screen_lock_band(true);

    decoder_start();
    ft8_log = adif_log_init(BOARD_DATA "/ft_log.adi");

    /* WSJT-X datagrams over UDP (GridTracker, JTAlert...): /mnt/ft8_udp.conf */
    ft8_udp_init();
    udp_checks       = 0;
    udp_sent.sending = -1;
    ft8_udp_send_heartbeat();
    udp_status();
    udp_check_timer = lv_timer_create(udp_check_cb, UDP_CHECK_MS, NULL);

    if (param_f_get(cfg_pwr) > TX_PLAYER_MAX_PWR_W) {
        radio_set_pwr(TX_PLAYER_MAX_PWR_W);
        msg_schedule_text_fmt("Power is limited to %0.0fW", TX_PLAYER_MAX_PWR_W);
    }
    /* TX gain to start from, for this base firmware and power */
    base_gain_offset = tx_player_base_gain_offset();   /* tx_player.c */
}

static void destruct_cb(void) {
    map_obj   = NULL;   /* deleted with the dialog */
    freq_list = NULL;
    kb_close();
    decoder_stop();
    win_gen++;      /* the decoder is joined: what it queued for this window goes nowhere now */
    /* After the decoder thread, which sends too */
    if (udp_check_timer) {
        lv_timer_del(udp_check_timer);
        udp_check_timer = NULL;
    }
    ft8_udp_deinit();
    table_view_destroy();

    dsp_set_waterfall_enabled(true);
    dsp_set_spectrum_enabled(true);

    main_screen_lock_mode(false);
    main_screen_lock_ab(false);
    main_screen_lock_freq(false);
    freq_strip_set_offset(-1);
    main_screen_lock_band(false);

    radio_set_pwr(param_f_get(cfg_pwr));
    adif_log_close(ft8_log);
}

static dialog_t dialog = {
    .run           = false,
    .construct_cb  = construct_cb,
    .destruct_cb   = destruct_cb,
    .audio_cb      = audio_cb,
    .rotary_cb     = rotary_cb,
    .key_cb        = key_cb,
    .restore_radio = true,
};

dialog_t *dialog_ft8 = &dialog;

/* ---- the browser panel ---- */

void ft8_window_status(ft8_status_t *s) {
    memset(s, 0, sizeof(*s));
    s->open     = dialog_type_is_run(dialog_ft8);
    s->proto    = cur_proto();
    s->tx_freq  = params.ft8_tx_freq.x;
    s->in_ms    = -1;       /* no countdown, no log: nothing ticks while the app is closed */
    s->logged_s = -1;
    s->slot_ms  = (int32_t)(ftx_slot_time(cur_proto()) * 1000);
    if (!s->open || !tx_enabled)
        return;
    s->cq      = subject_i_get(cq_enabled);
    s->tx      = subject_i_get(tx_enabled);
    s->auto_on = param_i_get(cfg_ft8_auto);
    s->hold    = param_i_get(cfg_ft8_hold_freq);
    s->sending = sending;

    /* This thread's (the panel's) view of the sequence, taken at once */
    ftx_tx_msg_t next;
    bool         odd_slots;

    qso_lock();
    next      = tx_msg;
    odd_slots = tx_odd;
    s->qso    = qso && ftx_qso_processor_has_current(qso);
    s->hound  = hound_on;
    qso_unlock();
    snprintf(s->next, sizeof(s->next), "%s", next.msg);
    if (!strncmp(s->next, "CQ_", 3)) s->next[2] = ' ';

    /* The timeline: when the queued message goes out, how often still, what was logged */
    float slot = ftx_slot_time(cur_proto());
    if (next.msg[0] && s->tx && !sending) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        float sec = (now.tv_sec % 60) + now.tv_nsec / 1.0e9f;
        int   n   = (int)(sec / slot);              /* this slot's number in the minute */
        bool  odd = (n % 2) != 0;
        float t;
        if (odd == odd_slots && fmodf(sec, slot) < tx_start_limit()) t = sec;           /* about to go, this slot */
        else t = (n + (odd == odd_slots ? 2 : 1)) * slot;                                 /* the next slot of our parity */
        s->in_ms = (int32_t)((t - sec) * 1000);
        if (s->in_ms < 0) s->in_ms = 0;
    }
    s->repeats = next.msg[0] ? next.repeats : 0;
    s->nocall  = !params.callsign.x[0];
    pthread_mutex_lock(&save_mux);
    snprintf(s->logged, sizeof(s->logged), "%s", last_logged);
    s->logged_s = last_logged[0] ? (int32_t)(time(NULL) - last_logged_at) : -1;
    pthread_mutex_unlock(&save_mux);
}

bool ft8_window_remote(ft8_rc_t what, int32_t arg, const cell_data_t *cell) {
    if (!dialog_type_is_run(dialog_ft8) || !tx_enabled)
        return false;
    switch (what) {
        case FT8_RC_CQ:     tx_cq_cb(NULL); break;
        case FT8_RC_STOP:
            /* CQ off here, not through tx_cq_cb: that does nothing while a keyboard is open on the radio */
            subject_i_set(cq_enabled, false);
            qso_lock();
            tx_msg.msg[0] = '\0';
            tx_gen++;
            qso_unlock();
            tx_call_off();      /* and the transmission under way */
            msg_schedule_text_fmt("Stopped: nothing transmits until TX Call, CQ or a call is pressed");
            break;
        case FT8_RC_TXCALL: tx_call_cb(NULL); break;
        case FT8_RC_AUTO:   auto_cb(NULL); break;
        case FT8_RC_HOLD:   hold_cb(NULL); break;
        case FT8_RC_MODE:   mode_cb(NULL); break;
        case FT8_RC_TXFREQ: set_tx_freq_in_filter(arg); break;
        case FT8_RC_REPLY:  if (cell) list_pressed(cell); break;
    }
    return true;
}
