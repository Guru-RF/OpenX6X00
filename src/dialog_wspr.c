/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - WSPR app
 *
 *  Two-minute cycles from the even UTC minute. In an RX cycle the audio is
 *  recorded (wspr/wspr.c) and decoded at 114 s, after the 110.6 s signal;
 *  the spots go to the table and, when Upload is on, to WSPRnet. A TX cycle
 *  (chosen at random with the TX % chance, or "TX next") sends the beacon
 *  from 1 s: call, 4-character locator and power. The power is WSPR's own
 *  (0.1 to 5 W, 0.2 W at first): the radio's power setting is left alone.
 *
 *  Threads: the cycle thread keeps the time and transmits (tx_player blocks
 *  for the whole beacon); each decode runs in a thread of its own, with a
 *  copy of the samples, so a slow decode never delays the next beacon.
 *  The UI is updated by a timer (state) and scheduler_put (spots).
 *
 *  Hop (page 2, #190): the band follows WSJT-X's coordinated slots
 *  (wspr/wspr_hop.c), each band set to Off, RX or RX+TX on page 3. Once a
 *  cycle is over (its beacon sent, its samples taken by the decode) the
 *  cycle thread asks the LVGL thread for the next cycle's band; no beacon is
 *  made until that is done. A band set to RX, or set to RX+TX with the ATU
 *  on and no match stored for it, never transmits. Each decode keeps its own
 *  cycle's dial frequency, for the table and WSPRnet, across a hop too.
 */

#include "dialog_wspr.h"

#include "app_frame.h"
#include "audio.h"
#include "buttons.h"
#include "cfg/atu_cache_api.h"
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
#include "wspr/wspr_hop.h"
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

#define MAX_ROWS        60
#define MAX_SPOTS       32

typedef enum {
    ST_WAIT,        /* opened mid-cycle: waits for the next one */
    ST_RX,
    ST_TX,
    ST_STOP,
} wspr_state_t;

enum {              /* tx_next_subj: the look of the TX next button */
    NEXT_OFF,
    NEXT_ARMED,
    NEXT_ON_AIR,    /* Stop TX */
};

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
static const char *pwr_label(void);
static void tx_pct_cb(button_data_t *b);
static void upload_cb(button_data_t *b);
static void tx_next_cb(button_data_t *b);
static void clear_cb(button_data_t *b);
static void pwr_cb(button_data_t *b);
static const char *hop_label(void);
static const char *band_label(void);
static const char *use_label(void);
static const char *goto_label(void);
static void hop_cb(button_data_t *b);
static void band_next_cb(button_data_t *b);
static void band_prev_cb(button_data_t *b);
static void use_cb(button_data_t *b);
static void goto_cb(button_data_t *b);
static void page_cb(button_data_t *b);
static void page_back_cb(button_data_t *b);

static buttons_page_t btn_page_1, btn_page_2, btn_page_3;

static button_data_t btn_page    = {.type = BTN_TEXT, .label = "(WSPR 1:3)", .press = page_cb, .hold = page_back_cb, .next = &btn_page_2, .prev = &btn_page_3};
static button_data_t btn_page2   = {.type = BTN_TEXT, .label = "(WSPR 2:3)", .press = page_cb, .hold = page_back_cb, .next = &btn_page_3, .prev = &btn_page_1};
static button_data_t btn_page3   = {.type = BTN_TEXT, .label = "(WSPR 3:3)", .press = page_cb, .hold = page_back_cb, .next = &btn_page_1, .prev = &btn_page_2};
static button_data_t btn_tx_pct  = {.type = BTN_TEXT_FN, .label_fn = tx_pct_label, .press = tx_pct_cb};
static button_data_t btn_tx_next = {.type = BTN_TEXT_FN, .label_fn = tx_next_label, .press = tx_next_cb};
static button_data_t btn_upload  = {.type = BTN_TEXT_FN, .label_fn = upload_label, .press = upload_cb};
static button_data_t btn_clear   = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};
static button_data_t btn_tx_pct2 = {.type = BTN_TEXT_FN, .label_fn = tx_pct_label, .press = tx_pct_cb};  /* TX % seen on page 2 too */
static button_data_t btn_pwr     = {.type = BTN_TEXT_FN, .label_fn = pwr_label, .press = pwr_cb};  /* WSPR's own, not the radio's */
static button_data_t btn_hop     = {.type = BTN_TEXT_FN, .label_fn = hop_label, .press = hop_cb};
static button_data_t btn_band    = {.type = BTN_TEXT_FN, .label_fn = band_label, .press = band_next_cb, .hold = band_prev_cb};
static button_data_t btn_use     = {.type = BTN_TEXT_FN, .label_fn = use_label, .press = use_cb};
static button_data_t btn_hop3    = {.type = BTN_TEXT_FN, .label_fn = hop_label, .press = hop_cb};  /* Hop seen on page 3 too */
static button_data_t btn_goto    = {.type = BTN_TEXT_FN, .label_fn = goto_label, .press = goto_cb};

static buttons_page_t btn_page_1 = {
    .items = {&btn_page, &btn_tx_pct, &btn_tx_next, &btn_upload, &btn_clear},
};

static buttons_page_t btn_page_2 = {
    .items = {&btn_page2, &btn_pwr, &btn_tx_pct2, &btn_ext_tune, &btn_hop},  /* TUNE: at the TX offset, at its own power */
};

/* The bands of the hop: the body shows their list while this page is up */
static buttons_page_t btn_page_3 = {
    .items = {&btn_page3, &btn_band, &btn_use, &btn_hop3, &btn_goto},
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
static float                tx_w;           /* its power, fixed with the dBm it reports */
static atomic_int           uploaded;
static atomic_int           upload_failed;
static atomic_int           decoding;
static uint32_t             dial_hz;        /* of the cycle being recorded */
static char                 my_call[16], my_grid[8];
static pthread_t            cycle_thread;
static bool                 cycle_thread_run;
/* Hop: whether the band the radio is on may carry a beacon (RX+TX, and a match stored when the ATU is on). The LVGL
 * thread works it out, the ATU cache is its own; the cycle thread reads it before each beacon */
static atomic_bool          band_tx_ok;
/* Hop: the next cycle's band asked of the LVGL thread and not there yet. No beacon is made meanwhile: it could be
 * one for a band the hop leaves, or for one that receives only */
static atomic_bool          hop_pending;

static SubjectInt          *tx_next_subj;

/* Page 3, the bands of the hop (LVGL thread) */
#define BAND_COLS 5
static lv_obj_t   *bands_table;
static bool        bands_shown;
static int         band_sel;                        /* the row F2 and the MFK move */
static int         band_now = -1;                   /* the band the radio is on, marked in the list */
static int32_t     band_freq[WSPR_HOP_BANDS];       /* each band's WSPR frequency in the app's list, 0 none */

static double now_s(void) {
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* A WSPR level in W, in the radio's 0.1 W steps: 20 dBm 0.1 W, 23 dBm 0.2 W, 27 0.5, 30 1, 33 2, 37 5 */
static float wspr_w(int dbm) {
    return roundf(powf(10.0f, dbm / 10.0f) / 100.0f) / 10.0f;
}

/* Hop */

static void tx_next_look_cb(void *arg);
static void bands_fill(void);

/* Any thread: whether a beacon may be made now. Hop off: always, as before */
static bool band_allows_tx(void) {
    return !param_i_get(cfg_wspr_hop) || (!atomic_load(&hop_pending) && atomic_load(&band_tx_ok));
}

/* Each band's WSPR frequency from the app's list (the web Digital modes page can change it): read again at each hop */
static void band_freqs_load(void) {
    cfg_digital_preset_t p[48];
    int32_t              f[48];
    int                  n = cfg_digital_list(CFG_DIG_TYPE_WSPR, p, 48);

    for (int i = 0; i < n; i++) {
        f[i] = p[i].freq;
    }
    wspr_hop_freqs(f, n, band_freq);
}

/* Whether band, at freq, may carry a beacon; no_match: it is set to RX+TX and only the ATU's missing match stops it */
static bool band_check(int band, int32_t freq, bool *no_match) {
    int32_t bands = param_i_get(cfg_wspr_bands);
    bool    atu   = param_i_get(cfg_atu_enabled);
    bool    match = atu && freq && cfg_atu_has_network_at(freq);

    if (no_match) {
        *no_match = atu && !match && wspr_hop_use(bands, band) == WSPR_USE_TX;
    }
    return wspr_hop_tx_ok(bands, band, atu, match);
}

/* LVGL thread, at each tick and after each change: band_tx_ok for the band the radio is on. With Hop on, a beacon on
 * the air on a band that may no longer carry one (Hop turned on there, its band set to RX, the ATU turned on without
 * a match) ends now, as with Stop TX but TX % kept: a band set to RX never transmits */
static void tx_ok_update(void) {
    int32_t f = cparam_i_get(cfg_fg_freq);
    int     b = wspr_hop_band_of(f);
    bool    no_match;
    bool    ok = band_check(b, f, &no_match);
    int     st = ST_TX;

    atomic_store(&band_tx_ok, ok);
    if (!ok && param_i_get(cfg_wspr_hop) && atomic_compare_exchange_strong(&state, &st, ST_WAIT)) {
        tx_next_look_cb(NULL);
        if (b < 0) {
            msg_update_text_fmt("WSPR: no band of the hop here, beacon stopped");
        } else if (no_match) {
            msg_update_text_fmt("WSPR: %dm: no ATU match stored, beacon stopped", wspr_hop_band_m(b));
        } else {
            msg_update_text_fmt("WSPR: %dm is %s with Hop on, beacon stopped", wspr_hop_band_m(b),
                                wspr_hop_use_name(wspr_hop_use(param_i_get(cfg_wspr_bands), b)));
        }
    }
}

/* LVGL thread: to the band of the cycle that starts at cycle. Off bands and bands the list has no frequency on are
 * left out; every band off: the app stays where it is */
static void hop_to(time_t cycle) {
    band_freqs_load();

    int32_t bands = wspr_hop_usable(param_i_get(cfg_wspr_bands), band_freq);
    int     next  = wspr_hop_pick(bands, cycle);
    bool    no_match;

    if (next >= 0 && next != wspr_hop_band_of(cparam_i_get(cfg_fg_freq)) &&
        !cfg_digital_load_at(band_freq[next], CFG_DIG_TYPE_WSPR)) {
        next = -1;
    }
    tx_ok_update();
    if (bands_shown) {
        bands_fill();
    }
    if (next >= 0 && !band_check(next, band_freq[next], &no_match) && no_match) {
        /* No tuning carrier of its own: the hop listens there. Tuning once on the band stores a match */
        msg_update_text_fmt("%dm: no ATU match stored, RX only", wspr_hop_band_m(next));
    }
}

typedef struct {
    unsigned gen;
    time_t   cycle;     /* the cycle the band is for */
} hop_req_t;

/* LVGL thread, asked by the cycle thread between two cycles. Late (into the next cycle), after a band key or a close:
 * no hop, so never in the middle of a cycle; the band is asked again after the next one */
static void hop_req_cb(void *arg) {
    hop_req_t *r  = arg;
    int        st = atomic_load(&state);

    if (dialog.run && r->gen == atomic_load(&gen) && param_i_get(cfg_wspr_hop) && st != ST_TX && st != ST_RX &&
        st != ST_STOP && now_s() < r->cycle - 0.5) {
        hop_to(r->cycle);
    }
    atomic_store(&hop_pending, false);
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

/* Polled every block (about 46 ms): Stop TX or a close ends the beacon there, and what is queued for the sound card
 * goes too, off the air at once instead of after its 0.8 s (as Winlink) */
static bool tx_abort_cb(void *ctx) {
    (void)ctx;
    if (atomic_load(&state) == ST_TX) {
        return false;
    }
    audio_play_flush();
    return true;
}

/* The TX next button's look from where the cycle is: Stop TX while a beacon is due or on the air. Scheduled by the
 * cycle thread when that changes, run on the LVGL thread with the state as it is then */
static void tx_next_look_cb(void *arg) {
    (void)arg;
    subject_i_set(tx_next_subj, atomic_load(&state) == ST_TX ? NEXT_ON_AIR
                                : atomic_load(&tx_next)       ? NEXT_ARMED
                                                              : NEXT_OFF);
}

static void tx_off_cb(void *arg) {
    (void)arg;
    param_i_set(cfg_wspr_tx_pct, 0);
    tx_next_look_cb(NULL);
}

/* The beacon for the next cycle, or NULL for RX. Its power goes with it (tx_w): a Power press before it goes out
 * cannot make it report one and send another */
static int16_t *prepare_tx(uint32_t *n) {
    uint8_t sym[WSPR_SYMBOLS];
    int     pct = param_i_get(cfg_wspr_tx_pct);
    int     dbm = param_i_get(cfg_wspr_dbm);

    tx_by_next = atomic_load(&tx_next);
    /* Hop: a band that receives only gets no beacon, nor one whose band is not settled yet. TX next stays armed for
     * the next band that may carry one */
    if (!band_allows_tx()) {
        return NULL;
    }
    if (!tx_by_next && (pct == 0 || rand() % 100 >= pct)) {
        return NULL;
    }
    tx_w = wspr_w(dbm);
    if (!wspr_encode(my_call, my_grid, dbm, sym)) {
        if (strlen(my_grid) < 4) {
            msg_schedule_text_fmt("WSPR TX: set the QTH locator in Settings");
        } else {
            msg_schedule_text_fmt("WSPR TX: \"%s %.4s\" can't be sent (plain call and locator only)", my_call, my_grid);
        }
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
    bool     hop_asked = false;

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
            } else if (tx && (!band_allows_tx() ||
                              (tx_by_next ? !atomic_load(&tx_next) : param_i_get(cfg_wspr_tx_pct) == 0))) {
                /* Prepared at 116 s, then what it was for turned off (TX next, TX % to 0, Stop TX), or with Hop on
                 * the band may no longer carry it (set to RX, a band key, the ATU on without a match): no beacon.
                 * One the TX % had chosen used to go out all the same */
                free(tx);
                tx = NULL;
            }
            prepared  = false;
            hop_asked = false;
            dial_hz   = cparam_i_get(cfg_fg_freq);
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
            scheduler_put_noargs(tx_next_look_cb);      /* Stop TX for a beacon due in this cycle */
        }

        int st = atomic_load(&state);

        if (st == ST_TX && t >= TX_START_S) {
            if (t <= TX_LATE_S) {
                atomic_store(&tx_next, false);
                tx_info_set_length(CYCLE_S);    /* the strip counts the cycle */
                /* WSPR's own power, on this thread: the radio's setting back after the beacon */
                tx_player_set_pwr(tx_w);
                tx_player_play(tx, tx_n, param_i_get(cfg_wspr_tx_freq), tx_player_base_gain_offset(), tx_abort_cb,
                               NULL);
                tx_player_set_pwr(0.0f);
            }
            free(tx);
            tx = NULL;
            atomic_compare_exchange_strong(&state, &st, ST_WAIT);
            scheduler_put_noargs(tx_next_look_cb);      /* back to TX next */
            continue;
        }
        if (st == ST_RX && t >= DECODE_S) {
            start_decode(cycle);
            atomic_compare_exchange_strong(&state, &st, ST_WAIT);
        }
        /* Hop: the next cycle's band, once this one is over. The decode has its samples and this cycle's dial
         * frequency already, so its spots and their upload stay on the band they were heard on */
        st = atomic_load(&state);
        if (!hop_asked && param_i_get(cfg_wspr_hop) && wspr_hop_window(t, st == ST_TX, st == ST_RX)) {
            hop_req_t r = {.gen = atomic_load(&gen), .cycle = cycle + CYCLE_S};

            hop_asked = true;
            atomic_store(&hop_pending, true);
            if (!scheduler_put(hop_req_cb, &r, sizeof(r))) {
                atomic_store(&hop_pending, false);  /* the queue is full: this band one more cycle */
            }
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
        int                band = wspr_hop_band_of(row->dial_hz);   /* the cycle's own, across hops too */

        gmtime_r(&row->cycle, &tm);
        lv_table_set_cell_value_fmt(table, x, 0, "%02d%02d", tm.tm_hour, tm.tm_min);
        if (band >= 0) {
            lv_table_set_cell_value_fmt(table, x, 1, "%dm", wspr_hop_band_m(band));
        } else {
            lv_table_set_cell_value(table, x, 1, "");
        }
        lv_table_set_cell_value_fmt(table, x, 2, "%d", s->snr);
        lv_table_set_cell_value_fmt(table, x, 3, "%.1f", roundf(s->dt * 10.0f) / 10.0f + 0.0f);   /* no "-0.0" */
        lv_table_set_cell_value_fmt(table, x, 4, "%.0f", s->freq_hz - row->dial_hz);   /* audio */
        lv_table_set_cell_value_fmt(table, x, 5, "%d", s->drift);
        lv_table_set_cell_value(table, x, 6, s->call);
        lv_table_set_cell_value(table, x, 7, s->loc);
        lv_table_set_cell_value_fmt(table, x, 8, "%d", s->dbm);
        if (row->km >= 0) {
            snprintf(buf, sizeof(buf), "%d", row->km);
        } else {
            buf[0] = 0;
        }
        lv_table_set_cell_value(table, x, 9, buf);
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

/* Page 3: the bands of the hop */

/* Each band's frequency, use, slots and whether the ATU has a match stored for it */
static void bands_fill(void) {
    static const char *heads[BAND_COLS] = {"Band", "MHz", "Use", "Slots", "ATU match"};
    int32_t            bands = param_i_get(cfg_wspr_bands);
    bool               atu   = param_i_get(cfg_atu_enabled);

    band_now = wspr_hop_band_of(cparam_i_get(cfg_fg_freq));
    lv_table_set_row_cnt(bands_table, WSPR_HOP_BANDS + 1);
    for (int c = 0; c < BAND_COLS; c++) {
        lv_table_set_cell_value(bands_table, 0, c, heads[c]);
    }
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        int        r   = i + 1;
        wspr_use_t use = wspr_hop_use(bands, i);

        lv_table_set_cell_value_fmt(bands_table, r, 0, "%dm", wspr_hop_band_m(i));
        if (band_freq[i]) {
            lv_table_set_cell_value_fmt(bands_table, r, 1, "%.4f", band_freq[i] / 1e6);
        } else {
            lv_table_set_cell_value(bands_table, r, 1, "none");     /* not in the list: the hop skips it */
        }
        lv_table_set_cell_value(bands_table, r, 2, wspr_hop_use_name(use));
        if (i < WSPR_HOP_SLOTS) {
            lv_table_set_cell_value_fmt(bands_table, r, 3, ":%02d :%02d :%02d", 2 * i, 2 * i + 20, 2 * i + 40);
        } else {
            lv_table_set_cell_value(bands_table, r, 3, "Off slots");   /* 6 m: none of its own */
        }
        if (!band_freq[i]) {
            lv_table_set_cell_value(bands_table, r, 4, "");
        } else if (cfg_atu_has_network_at(band_freq[i])) {
            lv_table_set_cell_value(bands_table, r, 4, "Stored");
        } else {
            lv_table_set_cell_value(bands_table, r, 4, atu && use == WSPR_USE_TX ? "None: RX only" : "None");
        }
    }
}

/* The row F2 and the MFK move: sel_bg; the band the radio is on: near_bg, as the cluster's spot at the VFO */
static void bands_draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) {
        return;
    }
    int row = (int)dsc->id / BAND_COLS - 1;
    int col = (int)dsc->id % BAND_COLS;

    if (row < 0) {
        return;
    }
    if (row == band_sel) {
        dsc->rect_dsc->bg_opa       = LV_OPA_COVER;
        dsc->rect_dsc->bg_color     = TC(sel_bg);
        dsc->label_dsc->color       = TC(sel_text);
        dsc->rect_dsc->border_color = TC(sel_border);
        dsc->rect_dsc->border_width = 2;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM |
                                     (col == 0 ? LV_BORDER_SIDE_LEFT : 0) | (col == BAND_COLS - 1 ? LV_BORDER_SIDE_RIGHT : 0);
    } else if (row == band_now) {
        dsc->rect_dsc->bg_opa   = LV_OPA_COVER;
        dsc->rect_dsc->bg_color = TC(near_bg);
        if (theme_shape.mono && col == 0) {     /* 1bit: near_bg is the paper; a bar on the left marks it */
            dsc->rect_dsc->border_color = TC(text);
            dsc->rect_dsc->border_width = 4;
            dsc->rect_dsc->border_opa   = LV_OPA_COVER;
            dsc->rect_dsc->border_side  = LV_BORDER_SIDE_LEFT;
        }
    }
}

/* The selected row in view, with the row over it: the body shows about seven of the twelve rows. Scrolled only as
 * far as that takes, so the headings stay in view while the top bands are chosen */
static void bands_show_sel(void) {
    lv_table_t *t   = (lv_table_t *)bands_table;
    lv_coord_t  top = 0, all = 0;

    for (int r = 0; r < (int)t->row_cnt; r++) {
        if (r < band_sel) {
            top += t->row_h[r];     /* the headings and the rows over the one above it */
        }
        all += t->row_h[r];
    }
    lv_coord_t bottom = top + t->row_h[band_sel] + t->row_h[band_sel + 1];
    lv_coord_t view   = lv_obj_get_content_height(bands_table);
    lv_coord_t y      = lv_obj_get_scroll_y(bands_table);

    if (bottom > y + view) {
        y = bottom - view;
    }
    if (top < y) {
        y = top;
    }
    y = LV_CLAMP(0, y, LV_MAX(0, all - view));
    lv_obj_scroll_to_y(bands_table, y, LV_ANIM_OFF);
}

static void bands_refresh(void) {
    bands_fill();
    bands_show_sel();
    lv_obj_invalidate(bands_table);
    buttons_refresh(&btn_band);
    buttons_refresh(&btn_use);
    buttons_refresh(&btn_goto);
}

/* The body follows the button page: the bands while page 3 is up, the spots on the others. From the page keys and
 * the timer, so a page loaded any other way is followed too */
static void view_follow_page(void) {
    bool show = buttons_get_cur_page() == &btn_page_3;

    if (!bands_table || show == bands_shown) {
        return;
    }
    bands_shown = show;
    if (show) {
        int b = wspr_hop_band_of(cparam_i_get(cfg_fg_freq));

        band_freqs_load();
        if (b >= 0) {
            band_sel = b;
        }
        lv_obj_add_flag(table, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bands_table, LV_OBJ_FLAG_HIDDEN);
        bands_refresh();
    } else {
        lv_obj_add_flag(bands_table, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(table, LV_OBJ_FLAG_HIDDEN);
    }
}

static void page_cb(button_data_t *b) {
    buttons_load_page(b->next);
    view_follow_page();
}

static void page_back_cb(button_data_t *b) {
    buttons_load_page(b->prev);
    view_follow_page();
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    static int ticks;
    double     now = now_s();
    double     s   = fmod(now, CYCLE_S);
    int        st  = atomic_load(&state);
    int        dbm = param_i_get(cfg_wspr_dbm);
    bool       hop = param_i_get(cfg_wspr_hop);

    tx_ok_update();
    view_follow_page();
    if (bands_shown && ++ticks % 4 == 0) {
        bands_fill();       /* once a second: the ATU on or off, a match just stored, the band after a hop */
    }
    lv_label_set_text_fmt(frame.head_left, "%s   %.4f MHz%s", cfg_digital_label_get(), cparam_i_get(cfg_fg_freq) / 1e6,
                          !hop ? "" : atomic_load(&band_tx_ok) ? "   Hop" : "   Hop, RX only");

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
    if (bands_shown) {
        /* The rule of the ATU column, under the list */
        lv_label_set_text(frame.status, param_i_get(cfg_atu_enabled)
                                            ? "ATU on: an RX+TX band without a stored match receives only"
                                            : "ATU off: every RX+TX band transmits on the antenna as it is");
        return;
    }
    lv_label_set_text_fmt(frame.status, "%sTX %d%% at %d Hz, %.1f W = %d dBm   %s", last, param_i_get(cfg_wspr_tx_pct),
                          param_i_get(cfg_wspr_tx_freq), wspr_w(dbm), dbm, up);
}

static const char *tx_pct_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "TX\n%d%%", param_i_get(cfg_wspr_tx_pct));
    return buf;
}

/* A beacon needs the callsign and 4 characters of locator: said at the press. TX next used to show armed and drop
 * at 1:56, when the beacon is made, with a message easy to miss (a radio with no QTH set and no GPS fix) */
static bool tx_possible(void) {
    if (!my_call[0]) {
        msg_update_text_fmt("WSPR TX: set the callsign in Settings");
        return false;
    }
    if (strlen(my_grid) < 4) {
        msg_update_text_fmt("WSPR TX: set the QTH locator in Settings");
        return false;
    }
    return true;
}

static void tx_pct_cb(button_data_t *b) {
    static const int steps[] = {0, 10, 20, 33, 50};
    int              cur = param_i_get(cfg_wspr_tx_pct);
    size_t           k   = 0;

    while (k < sizeof(steps) / sizeof(steps[0]) && steps[k] <= cur) {
        k++;
    }
    int next = k < sizeof(steps) / sizeof(steps[0]) ? steps[k] : 0;

    if (cur == 0 && next > 0 && !tx_possible()) {
        return;
    }
    param_i_set(cfg_wspr_tx_pct, next);
    if (cur == 0 && next > 0 && param_i_get(cfg_wspr_hop) && !wspr_hop_any(param_i_get(cfg_wspr_bands), WSPR_USE_TX)) {
        msg_update_text_fmt("WSPR Hop: no band is set to RX+TX (page 3), no beacon");
    }
}

static const char *pwr_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "Power\n%g W", wspr_w(param_i_get(cfg_wspr_dbm)));
    return buf;
}

/* WSPR's own power: the next level up, round to the lowest. The levels the 0.1 W steps reach exactly, to 5 W. The
 * shared Power button set the radio's power, for SSB and the other apps afterwards too */
static void pwr_cb(button_data_t *b) {
    static const int levels[] = {20, 23, 27, 30, 33, 37};
    int              cur = param_i_get(cfg_wspr_dbm);
    size_t           k   = 0;

    while (k < sizeof(levels) / sizeof(levels[0]) && levels[k] <= cur) {
        k++;
    }
    param_i_set(cfg_wspr_dbm, k < sizeof(levels) / sizeof(levels[0]) ? levels[k] : levels[0]);
}

static const char *tx_next_label(void) {
    switch (subject_i_get(tx_next_subj)) {
        case NEXT_ON_AIR:
            return "Stop TX";
        case NEXT_ARMED:
            return "TX next\n(armed)";
        default:
            return "TX next";
    }
}

/* Stop TX: the beacon off the air within a block, and none after it until TX % or TX next is pressed again. The rest
 * of the cycle is idle */
static void stop_tx(void) {
    int st = ST_TX;

    atomic_store(&tx_next, false);
    param_i_set(cfg_wspr_tx_pct, 0);
    atomic_compare_exchange_strong(&state, &st, ST_WAIT);   /* tx_abort_cb ends it */
    tx_next_look_cb(NULL);
    msg_update_text_fmt("WSPR: TX stopped, TX %% off");
}

/* While a beacon is due or on the air the button is Stop TX (page 1 has no room for one more). Before, a press
 * there armed another beacon for the next cycle: TX next is cleared as a beacon starts */
static void tx_next_cb(button_data_t *b) {
    if (atomic_load(&state) == ST_TX) {     /* also in the second before the audio starts */
        stop_tx();
        return;
    }

    bool on = !atomic_load(&tx_next);

    if (on && !tx_possible()) {
        return;
    }
    atomic_store(&tx_next, on);
    tx_next_look_cb(NULL);
    if (on && param_i_get(cfg_wspr_hop) && !atomic_load(&band_tx_ok)) {
        msg_update_text_fmt("WSPR Hop: receive only here, TX next waits for an RX+TX band");
    }
}

static const char *hop_label(void) {
    return param_i_get(cfg_wspr_hop) ? "Hop\nOn" : "Hop\nOff";
}

/* The band follows the slots from the end of this cycle, never in the middle of it */
static void hop_cb(button_data_t *b) {
    bool on = !param_i_get(cfg_wspr_hop);

    param_i_set(cfg_wspr_hop, on);
    band_freqs_load();
    if (!on) {
        msg_update_text_fmt("WSPR Hop off: %s stays", cfg_digital_label_get());
    } else if (!wspr_hop_any(wspr_hop_usable(param_i_get(cfg_wspr_bands), band_freq), WSPR_USE_RX)) {
        msg_update_text_fmt("WSPR Hop: every band is Off (page 3), it stays here");
    } else {
        msg_update_text_fmt("WSPR Hop on: the band follows the 2-minute slots from the next cycle");
    }
    tx_ok_update();     /* after: a beacon it stops says so */
}

static const char *band_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "Band\n%dm", wspr_hop_band_m(band_sel));
    return buf;
}

static void band_step(int dir) {
    band_sel = (band_sel + dir + WSPR_HOP_BANDS) % WSPR_HOP_BANDS;
    if (bands_shown) {
        bands_refresh();
    }
}

static void band_next_cb(button_data_t *b) {
    band_step(1);
}

static void band_prev_cb(button_data_t *b) {
    band_step(-1);
}

static const char *use_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "Use\n%s", wspr_hop_use_name(wspr_hop_use(param_i_get(cfg_wspr_bands), band_sel)));
    return buf;
}

/* The selected band: Off, RX, RX+TX. Kept at once, also with Hop off, so the bands can be set before it is on */
static void use_cb(button_data_t *b) {
    int32_t    bands = param_i_get(cfg_wspr_bands);
    wspr_use_t use   = wspr_hop_next_use(wspr_hop_use(bands, band_sel));
    bool       no_match;

    param_i_set(cfg_wspr_bands, wspr_hop_set_use(bands, band_sel, use));
    tx_ok_update();
    if (bands_shown) {
        bands_refresh();
    }
    band_check(band_sel, band_freq[band_sel], &no_match);
    if (!band_freq[band_sel]) {
        msg_update_text_fmt("%dm: no WSPR frequency in the list, the hop skips it", wspr_hop_band_m(band_sel));
    } else if (no_match) {
        msg_update_text_fmt("%dm: no ATU match stored, RX only", wspr_hop_band_m(band_sel));
    }
}

static const char *goto_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "Go to\n%dm", wspr_hop_band_m(band_sel));
    return buf;
}

static void restart_cycle(void);

/* The selected band now, as the band keys: to tune the ATU there (hold ATU) and store its match. With Hop on, the
 * next hop leaves it again */
static void goto_cb(button_data_t *b) {
    if (atomic_load(&state) == ST_TX) {
        msg_update_text_fmt("WSPR: wait for the end of the beacon");
        return;
    }
    if (!band_freq[band_sel]) {
        msg_update_text_fmt("%dm: no WSPR frequency in the list", wspr_hop_band_m(band_sel));
        return;
    }
    if (cfg_digital_load_at(band_freq[band_sel], CFG_DIG_TYPE_WSPR)) {
        msg_update_text_fmt("%s%s", cfg_digital_label_get(), param_i_get(cfg_wspr_hop) ? ", until the next hop" : "");
        restart_cycle();
        tx_ok_update();
        if (bands_shown) {
            bands_refresh();
        }
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
        msg_update_text_fmt("%s%s", cfg_digital_label_get(), param_i_get(cfg_wspr_hop) ? ", until the next hop" : "");
        restart_cycle();     /* this cycle was on the old band */
        tx_ok_update();
        if (bands_shown) {
            bands_refresh();
        }
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
            if (bands_shown) {
                band_step(-1);      /* page 3: the MFK moves the band row */
            } else {
                lv_obj_scroll_by_bounded(table, 0, 120, LV_ANIM_OFF);
            }
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            if (bands_shown) {
                band_step(1);
            } else {
                lv_obj_scroll_by_bounded(table, 0, -120, LV_ANIM_OFF);
            }
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
    /* Band: the cycle's own, which a hop changes from one row to the next */
    static const char      *heads[] = {"UTC", "Band", "dB", "DT", "Hz", "Dr", "Call", "Grid", "dBm", "km"};
    static const lv_coord_t widths[] = {75, 70, 50, 55, 70, 45, 150, 90, 60, 85};
    static const lv_coord_t band_widths[BAND_COLS] = {90, 130, 110, 190, 240};
    const char             *call = params.callsign.x;

    dialog.obj = dialog_init(parent);

    dsp_set_waterfall_enabled(false);
    dsp_set_spectrum_enabled(false);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    if (!tx_next_subj) {
        tx_next_subj    = subject_i_create(NEXT_OFF);
        btn_tx_next.subj = (Subject *)tx_next_subj;
    }
    btn_tx_pct.subj = (Subject *)cfg_wspr_tx_pct;
    btn_tx_pct2.subj = (Subject *)cfg_wspr_tx_pct;
    btn_upload.subj = (Subject *)cfg_wspr_upload;
    btn_pwr.subj = (Subject *)cfg_wspr_dbm;
    btn_hop.subj = (Subject *)cfg_wspr_hop;
    btn_hop3.subj = (Subject *)cfg_wspr_hop;
    btn_use.subj = (Subject *)cfg_wspr_bands;      /* the web Digital modes page can change it too */
    /* TX % is saved: off at every start, also after a power cut with WSPR open, so no beacon goes out unasked */
    param_i_set(cfg_wspr_tx_pct, 0);
    atomic_store(&tx_next, false);
    subject_i_set(tx_next_subj, NEXT_OFF);
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
    lv_table_set_col_cnt(table, 10);
    for (int c = 0; c < 10; c++) {
        lv_table_set_col_width(table, c, widths[c]);
        lv_table_set_cell_value(table, 0, c, heads[c]);
    }
    table_fill();

    /* Page 3's band list, in the same place, shown while that page is up */
    bands_table = lv_table_create(dialog.obj);
    lv_obj_remove_style(bands_table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    app_frame_body(bands_table);
    lv_obj_set_style_border_width(bands_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bands_table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_font(bands_table, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bands_table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(bands_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(bands_table, 5, LV_PART_ITEMS);
    lv_table_set_col_cnt(bands_table, BAND_COLS);
    for (int c = 0; c < BAND_COLS; c++) {
        lv_table_set_col_width(bands_table, c, band_widths[c]);
    }
    lv_obj_add_event_cb(bands_table, bands_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_flag(bands_table, LV_OBJ_FLAG_HIDDEN);
    bands_shown = false;
    band_freqs_load();


    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    if (cfg_digital_load(0, CFG_DIG_TYPE_WSPR)) {
        msg_update_text_fmt("%s", cfg_digital_label_get());
    }
    atomic_store(&hop_pending, false);
    if (param_i_get(cfg_wspr_hop)) {
        /* Straight to the slot's band of the first cycle it records: this one if it opens in its first second, else
         * the next, the rest of this one waits anyway. The cycle thread is not running yet */
        double now = now_s();
        time_t c   = (time_t)(now / CYCLE_S) * CYCLE_S;

        hop_to(now - c < TX_START_S ? c : c + CYCLE_S);
    }
    tx_ok_update();
    main_screen_lock_ab(true);
    main_screen_lock_mode(true);
    main_screen_lock_freq(true);
    main_screen_lock_band(true);
    freq_strip_set_offset(param_i_get(cfg_wspr_tx_freq));

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
    param_i_set(cfg_wspr_tx_pct, 0);
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
    table       = NULL;
    bands_table = NULL;     /* both go with the dialog's object */
    bands_shown = false;
}
