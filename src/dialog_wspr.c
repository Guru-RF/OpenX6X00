/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - WSPR app
 *
 *  Two-minute cycles from the even UTC minute. In an RX cycle the audio is
 *  recorded (wspr/wspr.c) and decoded at 114 s, after the 110.6 s signal;
 *  the spots go to the table and, when Upload is on, to WSPRnet. A TX cycle
 *  (chosen at random with the TX % chance, or "TX next") sends the beacon
 *  from 1 s: call, 4-character locator and power, at most 7 W.
 *
 *  Threads: the cycle thread keeps the time and transmits (tx_player blocks
 *  for the whole beacon); each decode runs in a thread of its own, with a
 *  copy of the samples, so a slow decode never delays the next beacon.
 *  The UI is updated by a timer (state) and scheduler_put (spots).
 */

#include "dialog_wspr.h"

#include "app_frame.h"
#include "audio.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cfg/digital_modes.h"
#include "dialog.h"
#include "dsp.h"
#include "events.h"
#include "freq_strip.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "params/params.h"
#include "qth/qth.h"
#include "radio.h"
#include "scheduler.h"
#include "station.h"
#include "styles.h"
#include "theme.h"
#include "tx_player.h"
#include "tx_info.h"
#include "wspr/wspr.h"
#include "wspr/wsprnet.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_RATE     (AUDIO_CAPTURE_RATE / AUDIO_DECIM)
#define CYCLE_S         120
#define TX_START_S      1.0     /* the signal starts 1 s into the cycle */
#define TX_LATE_S       2.0     /* later than this the beacon waits a cycle */
#define DECODE_S        114.0   /* the signal ends at 111.6 s (+ drift) */
#define PREPARE_S       116.0   /* next cycle chosen and synthesised */
#define MAX_PWR         TX_PLAYER_MAX_PWR_W

#define MAX_ROWS        60
#define MAX_SPOTS       32

typedef enum {
    ST_WAIT,        /* opened mid-cycle: waits for the next one */
    ST_RX,
    ST_TX,
    ST_STOP,
} wspr_state_t;

typedef struct {
    time_t      cycle;          /* UTC start */
    uint32_t    dial_hz;
    wspr_spot_t spot;
    int         km;             /* -1 unknown */
} row_t;

/* A finished decode, from its thread to the UI */
typedef struct {
    unsigned    gen;
    time_t      cycle;
    uint32_t    dial_hz;
    float       secs;           /* decode time */
    int         n;
    wspr_spot_t spots[MAX_SPOTS];
} decoded_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void audio_cb(unsigned int n, float *samples);
static void rotary_cb(int32_t diff);

static const char *tx_pct_label(void);
static const char *upload_label(void);
static const char *tx_next_label(void);
static void tx_pct_cb(button_data_t *b);
static void upload_cb(button_data_t *b);
static void tx_next_cb(button_data_t *b);
static void clear_cb(button_data_t *b);

static buttons_page_t btn_page_1, btn_page_2;

static button_data_t btn_page    = {.type = BTN_TEXT, .label = "(WSPR 1:2)", .press = button_next_page_cb, .hold = button_prev_page_cb, .next = &btn_page_2, .prev = &btn_page_2};
static button_data_t btn_page2   = {.type = BTN_TEXT, .label = "(WSPR 2:2)", .press = button_next_page_cb, .hold = button_prev_page_cb, .next = &btn_page_1, .prev = &btn_page_1};
static button_data_t btn_tx_pct  = {.type = BTN_TEXT_FN, .label_fn = tx_pct_label, .press = tx_pct_cb};
static button_data_t btn_tx_next = {.type = BTN_TEXT_FN, .label_fn = tx_next_label, .press = tx_next_cb};
static button_data_t btn_upload  = {.type = BTN_TEXT_FN, .label_fn = upload_label, .press = upload_cb};
static button_data_t btn_clear   = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};
static button_data_t btn_tx_pct2 = {.type = BTN_TEXT_FN, .label_fn = tx_pct_label, .press = tx_pct_cb};  /* TX % seen on page 2 too */

static buttons_page_t btn_page_1 = {
    .items = {&btn_page, &btn_tx_pct, &btn_tx_next, &btn_upload, &btn_clear},
};

static buttons_page_t btn_page_2 = {
    .items = {&btn_page2, &btn_dig_pwr, &btn_tx_pct2},
};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = audio_cb,
    .rotary_cb    = rotary_cb,
    .key_cb       = key_cb,
    .restore_radio = true,
};

dialog_t *dialog_wspr = &dialog;

static app_frame_t frame;
static lv_obj_t   *table;
static lv_timer_t *timer;

static row_t       rows[MAX_ROWS];
static int         rows_n;
static double      my_lat, my_lon;
static int         last_n = -1;    /* spots of the last decode, -1 none yet */
static float       last_secs;
static bool        my_pos;

/* Shared with the threads */
static wspr_bb_t           *bb;             /* kept for good: the audio thread may still feed it */
static atomic_int           state = ST_STOP;
static atomic_uint          gen;            /* bumped on close and band change */
static atomic_bool          tx_next;
static bool                 tx_by_next;    /* the prepared beacon is there for TX next, not the TX % pick */
static atomic_int           uploaded;
static atomic_int           upload_failed;
static atomic_int           decoding;
static uint32_t             dial_hz;        /* of the cycle being recorded */
static float                base_gain_offset;
static char                 my_call[16], my_grid[8];
static pthread_t            cycle_thread;
static bool                 cycle_thread_run;

static SubjectInt          *tx_next_subj;

static double now_s(void) {
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Decode thread */

typedef struct {
    unsigned gen;
    time_t   cycle;
    uint32_t dial_hz;
    unsigned n;
    float    *i, *q;
} decode_job_t;

static void decoded_cb(void *arg);

static void *decode_thread(void *arg) {
    decode_job_t *job = arg;
    decoded_t     res = {.gen = job->gen, .cycle = job->cycle, .dial_hz = job->dial_hz};

    double t0 = now_s();

    res.n    = wspr_decode_iq(job->i, job->q, job->n, job->dial_hz, res.spots, MAX_SPOTS);
    res.secs = now_s() - t0;
    free(job->i);
    free(job->q);
    atomic_store(&decoding, 0);
    scheduler_put(decoded_cb, &res, sizeof(res));

    if (param_i_get(cfg_wspr_upload) && my_call[0] && my_grid[0]) {
        int ok = wsprnet_post(my_call, my_grid, job->dial_hz, job->cycle, res.spots, res.n);

        if (ok < (res.n ? res.n : 1)) {
            atomic_fetch_add(&upload_failed, 1);
        }
        atomic_fetch_add(&uploaded, res.n ? ok : 0);
    }
    free(job);
    return NULL;
}

static void start_decode(time_t cycle) {
    decode_job_t  *job = calloc(1, sizeof(*job));
    pthread_t      t;
    pthread_attr_t attr;

    job->gen     = atomic_load(&gen);
    job->cycle   = cycle;
    job->dial_hz = dial_hz;
    job->i       = malloc(sizeof(float) * WSPR_SAMPLES);
    job->q       = malloc(sizeof(float) * WSPR_SAMPLES);
    job->n       = wspr_bb_take(bb, job->i, job->q);

    /* wsprd keeps big tables on the stack */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    atomic_store(&decoding, 1);
    if (pthread_create(&t, &attr, decode_thread, job) != 0) {
        atomic_store(&decoding, 0);
        free(job->i);
        free(job->q);
        free(job);
    }
    pthread_attr_destroy(&attr);
}

/* Cycle thread */

static bool tx_abort_cb(void *ctx) {
    (void)ctx;
    return atomic_load(&state) != ST_TX;
}

static void tx_next_clear_cb(void *arg) {
    (void)arg;
    subject_i_set(tx_next_subj, false);
}

static void tx_off_cb(void *arg) {
    (void)arg;
    param_i_set(cfg_wspr_tx_pct, 0);
    subject_i_set(tx_next_subj, false);
}

/* The beacon for the next cycle, or NULL for RX */
static int16_t *prepare_tx(uint32_t *n) {
    uint8_t sym[WSPR_SYMBOLS];
    int     pct = param_i_get(cfg_wspr_tx_pct);
    float   pwr = fminf(param_f_get(cfg_pwr), MAX_PWR);

    tx_by_next = atomic_load(&tx_next);
    if (!tx_by_next && (pct == 0 || rand() % 100 >= pct)) {
        return NULL;
    }
    if (!wspr_encode(my_call, my_grid, wspr_dbm(pwr), sym)) {
        msg_schedule_text_fmt("WSPR TX: \"%s %.4s\" can't be sent (plain call and locator only)", my_call, my_grid);
        atomic_store(&tx_next, false);
        scheduler_put_noargs(tx_off_cb);
        return NULL;
    }
    return wspr_synth(sym, TX_PLAYER_AUDIO_HZ, AUDIO_PLAY_RATE, n);
}

static void *cycle_thread_cb(void *arg) {
    (void)arg;
    time_t   cycle = 0;
    int16_t *tx = NULL;
    uint32_t tx_n = 0;
    bool     prepared = false;

    while (atomic_load(&state) != ST_STOP) {
        double now = now_s();
        time_t c   = (time_t)(now / CYCLE_S) * CYCLE_S;
        double t   = now - c;

        if (c != cycle) {
            /* New cycle: a beacon if one was prepared, else record */
            cycle = c;
            if (!prepared) {
                free(tx);
                tx = prepare_tx(&tx_n);
            } else if (tx && tx_by_next && !atomic_load(&tx_next)) {
                /* Prepared at 116 s for TX next, then TX next was turned off: no beacon */
                free(tx);
                tx = NULL;
            }
            prepared = false;
            dial_hz  = cparam_i_get(cfg_fg_freq);
            wspr_bb_reset(bb);

            int old = atomic_load(&state);
            int next = tx ? ST_TX : t < TX_START_S ? ST_RX : ST_WAIT;

            /* Never over ST_STOP (the dialog is closing); a change from the
             * UI in between (band, TX next) just makes it try again */
            while (old != ST_STOP && !atomic_compare_exchange_weak(&state, &old, next)) {
            }
            if (old == ST_STOP) {
                break;
            }
        }

        int st = atomic_load(&state);

        if (st == ST_TX && t >= TX_START_S) {
            if (t <= TX_LATE_S) {
                atomic_store(&tx_next, false);
                scheduler_put_noargs(tx_next_clear_cb);
                tx_info_set_length(CYCLE_S);    /* the strip counts the cycle */
                tx_player_play(tx, tx_n, param_i_get(cfg_wspr_tx_freq), base_gain_offset, tx_abort_cb, NULL);
            }
            free(tx);
            tx = NULL;
            atomic_compare_exchange_strong(&state, &st, ST_WAIT);
            continue;
        }
        if (st == ST_RX && t >= DECODE_S) {
            start_decode(cycle);
            atomic_compare_exchange_strong(&state, &st, ST_WAIT);
        }
        if (!prepared && t >= PREPARE_S && atomic_load(&state) != ST_STOP) {
            free(tx);
            tx       = prepare_tx(&tx_n);
            prepared = true;
        }
        usleep(100000);
    }
    free(tx);
    return NULL;
}

/* Audio thread */

static void audio_cb(unsigned int n, float *samples) {
    if (atomic_load(&state) == ST_RX) {
        wspr_bb_feed(bb, samples, n);
    }
}

/* UI */

static void table_fill(void) {
    char      buf[32];
    struct tm tm;

    lv_table_set_row_cnt(table, rows_n + 1);
    for (int r = 0; r < rows_n; r++) {
        const row_t       *row = &rows[r];
        const wspr_spot_t *s   = &row->spot;
        int                x   = r + 1;

        gmtime_r(&row->cycle, &tm);
        lv_table_set_cell_value_fmt(table, x, 0, "%02d%02d", tm.tm_hour, tm.tm_min);
        lv_table_set_cell_value_fmt(table, x, 1, "%d", s->snr);
        lv_table_set_cell_value_fmt(table, x, 2, "%.1f", roundf(s->dt * 10.0f) / 10.0f + 0.0f);   /* no "-0.0" */
        lv_table_set_cell_value_fmt(table, x, 3, "%.0f", s->freq_hz - row->dial_hz);   /* audio */
        lv_table_set_cell_value_fmt(table, x, 4, "%d", s->drift);
        lv_table_set_cell_value(table, x, 5, s->call);
        lv_table_set_cell_value(table, x, 6, s->loc);
        lv_table_set_cell_value_fmt(table, x, 7, "%d", s->dbm);
        if (row->km >= 0) {
            snprintf(buf, sizeof(buf), "%d", row->km);
        } else {
            buf[0] = 0;
        }
        lv_table_set_cell_value(table, x, 8, buf);
    }
}

static void decoded_cb(void *arg) {
    decoded_t *res = arg;

    if (!dialog.run || res->gen != atomic_load(&gen)) {
        return;
    }

    last_n    = res->n;
    last_secs = res->secs;
    if (res->n == 0) {
        return;
    }
    /* Newest first */
    int keep = MAX_ROWS - res->n;

    if (keep < 0) {
        keep = 0;
    }
    if (rows_n > keep) {
        rows_n = keep;
    }
    memmove(&rows[res->n], &rows[0], sizeof(row_t) * rows_n);
    for (int k = 0; k < res->n && k < MAX_ROWS; k++) {
        row_t *row = &rows[k];
        double lat, lon;

        row->cycle   = res->cycle;
        row->dial_hz = res->dial_hz;
        row->spot  = res->spots[k];
        row->km    = -1;
        if (my_pos && strlen(row->spot.loc) >= 4) {
            qth_str_to_pos(row->spot.loc, &lat, &lon);
            row->km = (int)qth_pos_dist(my_lat, my_lon, lat, lon);
        }
    }
    rows_n += res->n;
    if (rows_n > MAX_ROWS) {
        rows_n = MAX_ROWS;
    }
    table_fill();
    lv_obj_scroll_to_y(table, 0, LV_ANIM_OFF);
    msg_update_text_fmt("WSPR: %d spot%s", res->n, res->n == 1 ? "" : "s");
}

void dialog_wspr_show_spots(const wspr_spot_t *spots, int n) {
    decoded_t res = {.gen = atomic_load(&gen), .cycle = (time_t)(now_s() / CYCLE_S) * CYCLE_S,
                     .dial_hz = cparam_i_get(cfg_fg_freq), .n = n};

    memcpy(res.spots, spots, sizeof(wspr_spot_t) * (n < MAX_SPOTS ? n : MAX_SPOTS));
    decoded_cb(&res);
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    double now = now_s();
    double s   = fmod(now, CYCLE_S);
    int    st  = atomic_load(&state);
    float  pwr = fminf(param_f_get(cfg_pwr), MAX_PWR);

    lv_label_set_text_fmt(frame.head_left, "%s   %.4f MHz", cfg_digital_label_get(), cparam_i_get(cfg_fg_freq) / 1e6);

    switch (st) {
        case ST_RX:
            lv_label_set_text_fmt(frame.head_right, "RX  %d:%02d", (int)s / 60, (int)s % 60);
            break;
        case ST_TX:
            lv_label_set_text_fmt(frame.head_right, "#%06x TX#  %d:%02d", theme_hex(TC_tx), (int)s / 60, (int)s % 60);
            break;
        default:
            lv_label_set_text_fmt(frame.head_right, "%s  %d:%02d", atomic_load(&decoding) ? "Decoding" : "Wait",
                                  (int)(CYCLE_S - s) / 60, (int)(CYCLE_S - s) % 60);
            break;
    }
    app_frame_bar(&frame, (int)(s * 1000 / CYCLE_S), st == ST_TX ? theme_hex(TC_tx) : st == ST_RX ? theme_hex(TC_ok_bar) : theme_hex(TC_text_faint));

    char up[48] = "Upload off";

    if (param_i_get(cfg_wspr_upload)) {
        if (!my_call[0]) {
            snprintf(up, sizeof(up), "Upload: no callsign");
        } else if (atomic_load(&upload_failed)) {
            snprintf(up, sizeof(up), "Uploaded %d, #%06x %d failed#", atomic_load(&uploaded), theme_hex(TC_err),
                     atomic_load(&upload_failed));
        } else {
            snprintf(up, sizeof(up), "Uploaded %d", atomic_load(&uploaded));
        }
    }
    char last[40] = "";

    if (last_n >= 0) {
        snprintf(last, sizeof(last), "Last: %d spot%s (%.1f s)   ", last_n, last_n == 1 ? "" : "s", last_secs);
    }
    lv_label_set_text_fmt(frame.status, "%sTX %d%% at %d Hz, %.1f W = %d dBm   %s", last, param_i_get(cfg_wspr_tx_pct),
                          param_i_get(cfg_wspr_tx_freq), pwr, wspr_dbm(pwr), up);
}

static const char *tx_pct_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "TX\n%d%%", param_i_get(cfg_wspr_tx_pct));
    return buf;
}

static void tx_pct_cb(button_data_t *b) {
    static const int steps[] = {0, 10, 20, 33, 50};
    int              cur = param_i_get(cfg_wspr_tx_pct);
    size_t           k   = 0;

    while (k < sizeof(steps) / sizeof(steps[0]) && steps[k] <= cur) {
        k++;
    }
    param_i_set(cfg_wspr_tx_pct, k < sizeof(steps) / sizeof(steps[0]) ? steps[k] : 0);
}

static const char *tx_next_label(void) {
    return subject_i_get(tx_next_subj) ? "TX next\n(armed)" : "TX next";
}

static void tx_next_cb(button_data_t *b) {
    bool on = !atomic_load(&tx_next);

    if (on && !my_call[0]) {
        msg_update_text_fmt("WSPR TX: set the callsign in Settings");
        return;
    }
    atomic_store(&tx_next, on);
    subject_i_set(tx_next_subj, on);
    if (!on) {
        int st = ST_TX;

        atomic_compare_exchange_strong(&state, &st, ST_WAIT);  /* stops the beacon on the air */
    }
}

static const char *upload_label(void) {
    return param_i_get(cfg_wspr_upload) ? "Upload\nOn" : "Upload\nOff";
}

static void upload_cb(button_data_t *b) {
    param_i_set(cfg_wspr_upload, !param_i_get(cfg_wspr_upload));
}

static void clear_cb(button_data_t *b) {
    rows_n = 0;
    table_fill();
}

static void restart_cycle(void) {
    int st = ST_RX;

    atomic_fetch_add(&gen, 1);
    atomic_compare_exchange_strong(&state, &st, ST_WAIT);
}

static void band_cb(lv_event_t *e) {
    /* tx_player gives the old frequency back at the end of the beacon */
    if (atomic_load(&state) == ST_TX) {
        msg_update_text_fmt("WSPR: wait for the end of the beacon");
        return;
    }
    if (cfg_digital_load(lv_event_get_code(e) == EVENT_BAND_UP ? 1 : -1, CFG_DIG_TYPE_WSPR)) {
        msg_update_text_fmt("%s", cfg_digital_label_get());
        restart_cycle();     /* this cycle was on the old band */
    }
}

static void rotary_cb(int32_t diff) {
    int f = param_i_get(cfg_wspr_tx_freq) + diff;

    f = f < 1410 ? 1410 : f > 1590 ? 1590 : f;
    param_i_set(cfg_wspr_tx_freq, f);
    freq_strip_set_offset(f);
    msg_update_text_fmt("WSPR TX at %d Hz", f);
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            lv_obj_scroll_by_bounded(table, 0, 120, LV_ANIM_OFF);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            lv_obj_scroll_by_bounded(table, 0, -120, LV_ANIM_OFF);
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

static void construct_cb(lv_obj_t *parent) {
    static const char      *heads[] = {"UTC", "dB", "DT", "Hz", "Dr", "Call", "Grid", "dBm", "km"};
    static const lv_coord_t widths[] = {75, 55, 60, 90, 50, 150, 95, 70, 120};
    const char             *call = params.callsign.x;

    dialog.obj = dialog_init(parent);

    dsp_set_waterfall_enabled(false);
    dsp_set_spectrum_enabled(false);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    if (!tx_next_subj) {
        tx_next_subj    = subject_i_create(false);
        btn_tx_next.subj = (Subject *)tx_next_subj;
    }
    btn_tx_pct.subj = (Subject *)cfg_wspr_tx_pct;
    btn_tx_pct2.subj = (Subject *)cfg_wspr_tx_pct;
    btn_upload.subj = (Subject *)cfg_wspr_upload;
    atomic_store(&tx_next, false);
    subject_i_set(tx_next_subj, false);
    buttons_load_page(&btn_page_1);

    app_frame_create(&frame, dialog.obj);

    table = lv_table_create(dialog.obj);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    app_frame_body(table);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(table, 5, LV_PART_ITEMS);
    lv_table_set_col_cnt(table, 9);
    for (int c = 0; c < 9; c++) {
        lv_table_set_col_width(table, c, widths[c]);
        lv_table_set_cell_value(table, 0, c, heads[c]);
    }
    table_fill();


    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    if (cfg_digital_load(0, CFG_DIG_TYPE_WSPR)) {
        msg_update_text_fmt("%s", cfg_digital_label_get());
    }
    main_screen_lock_ab(true);
    main_screen_lock_mode(true);
    main_screen_lock_freq(true);
    main_screen_lock_band(true);
    freq_strip_set_offset(param_i_get(cfg_wspr_tx_freq));

    if (param_f_get(cfg_pwr) > MAX_PWR) {
        radio_set_pwr(MAX_PWR);
        msg_update_text_fmt("Power limited to %.0f W", MAX_PWR);
    }
    base_gain_offset = tx_player_base_gain_offset();

    /* Uppercase call, 6-character locator (WSPRnet), 4 of them go on the air */
    snprintf(my_call, sizeof(my_call), "%s", call ? call : "");
    for (char *p = my_call; *p; p++) {
        *p = toupper((unsigned char)*p);
    }
    snprintf(my_grid, sizeof(my_grid), "%s", station_qth());
    my_pos = strlen(my_grid) >= 4;
    if (my_pos) {
        qth_str_to_pos(my_grid, &my_lat, &my_lon);
    }

    if (!bb) {
        bb = wspr_bb_create(SAMPLE_RATE);
    }
    last_n = -1;
    atomic_store(&uploaded, 0);
    atomic_store(&upload_failed, 0);
    atomic_fetch_add(&gen, 1);
    atomic_store(&state, ST_WAIT);
    srand(time(NULL));
    cycle_thread_run = pthread_create(&cycle_thread, NULL, cycle_thread_cb, NULL) == 0;

    timer_cb(NULL);
    timer = lv_timer_create(timer_cb, 250, NULL);
}

static void destruct_cb(void) {
    /* Stops the beacon (tx_player checks every block) and the recording */
    atomic_store(&state, ST_STOP);
    atomic_fetch_add(&gen, 1);
    if (cycle_thread_run) {
        pthread_join(cycle_thread, NULL);
        cycle_thread_run = false;
    }
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    dsp_set_waterfall_enabled(true);
    dsp_set_spectrum_enabled(true);
    main_screen_lock_mode(false);
    main_screen_lock_ab(false);
    main_screen_lock_freq(false);
    main_screen_lock_band(false);
    freq_strip_set_offset(-1);
    radio_set_pwr(param_f_get(cfg_pwr));
    table = NULL;
}
