// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *  Xiegu X6100 LVGL GUI - the FT8 / FT4 / FT2 window
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  Waterfall with the TX frequency marker and the calls heard over it (the lane), the message list over it
 *  (ft8/decode_list.c) or the stations heard as tiles (ft8/heard.c), five pages of buttons; the lane, the tiles and
 *  the pages' order are F4OLI's redesign (OlivLoze's fork). Decoding runs in ft8/audio_worker.c; its callbacks
 *  below run on that thread and reach LVGL only through scheduler_put(). The QSO sequence is ft8/qso.cpp's,
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
#include "remote_server.h"
#include "scheduler.h"
#include "station.h"
#include "voice.h"
#include "gui_styles.h"
#include "text_popup.h"
#include "theme.h"
#include "tx_player.h"
#include "util.h"
#include "btaudio.h"   /* sh_run */
#include "clock_sync.h"
#include "ft8/audio_worker.h"
#include "ft8/cq_scheduler.h"
#include "ft8/ft2.h"
#include "ft8/hound.h"
#include "ft8/msg_encode.h"
#include "ft8/qso.h"
#include "ft8/decode_list.h"
#include "ft8/heard.h"
#include "ft8/time_sync.h"
#include "ft8/tx_worker.h"
#include "ft8/utils.h"
#include "ft8/worker.h"
#include "widgets/tune_marker.h"
#include "widgets/app_waterfall.h"

#define SAMPLE_RATE     (AUDIO_CAPTURE_RATE / AUDIO_DECIM)

#define WIDTH           771
#define WF_HEIGHT       (DIALOG_H - 26)   /* 13 px margins above and below */
#define WF_VISIBLE      40     /* waterfall clear above the log: ~16 s, a slot, to find a clear spot */
#define LANE_H          26     /* the calls over the waterfall, on two lines */
#define LIST_Y          (13 + LANE_H + WF_VISIBLE)
#define LIST_H          (WF_HEIGHT - LANE_H - WF_VISIBLE)

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
static const proto_info_t *proto_of(int p) {
    return &protos[p == FTX_PROTOCOL_FT8 || p == FTX_PROTOCOL_FT4 ? p : (int)FTX_PROTOCOL_FT2];
}

static const proto_info_t *proto(void) {
    return proto_of(cur_proto());
}

static const char *mode_name(void) {
    return ftx_protocol_name(cur_proto());
}

static bool is_ft2(void) {
    return cur_proto() == (int)FTX_PROTOCOL_FT2;
}

/* A transmission may start this late into its slot: FT2 lasts 2.52 s and must end inside 3.75 s. FT8 and FT4 leave
 * out what is past 0.5 s (tx_worker.c): up to 1.0 s of FT8's 12.64 s, 0.5 s of FT4's 5.04 s, and the time of the
 * slot's last decode on top (the sec checked is read before it); past a fifth tx_worker.c does not key */
static float tx_start_limit(void) {
    return is_ft2() ? 0.8f : cur_proto() == (int)FTX_PROTOCOL_FT4 ? 1.0f : 1.5f;
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
/* The dial and the mode the decoder's audio is of, from just before a change of either here
 * until the decoder restarts: a QSO its last decodes complete is logged with them, not the new
 * ones (rx_hold). Under qso_mux */
static bool            rx_held;
static int64_t         rx_dial;
static int             rx_proto;

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
static int              dec_proto;  /* the mode the decoder runs in: set before its thread starts */
static FTxQsoProcessor *qso;
static adif_log         ft8_log;

static double  my_lat, my_lon;
static int32_t filter_low, filter_high;
static float   base_gain_offset;

static button_data_t button_freq, button_hound, button_foryou;

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

static void lane_update(void);
static void board_update(void);

/* A decode with its sender: NEW when its country is not in the log, and its station on the tiles and the lane, which
 * so take what the list shows (Show: CQ leaves the other QSOs out of all three) */
static void row_apply(void *arg) {
    row_job_t *j = arg;

    if (j->win_gen != win_gen) return;
    if (j->cd.cell_type == CELL_RX_CQ || j->cd.cell_type == CELL_RX_MSG || j->cd.cell_type == CELL_RX_TO_ME) {
        j->cd.new_dxcc = j->cd.cty_prefix[0] && !ft8_worked_has(j->cd.cty_prefix);
        ft8_heard_add(&j->cd, lv_tick_get());
        lane_update();
        board_update();
    }
    table_view_add_msg_cb(&j->cd);
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

/* ---- The countries of the log: a CQ or a call from another one is NEW (the list's tag, the tiles) ---- */

static void worked_add_call(const char *call, void *ud) {
    const char *name, *prefix;

    (void)ud;
    if (dxcc_lookup(call, &name, &prefix)) ft8_worked_add(prefix);
}

/* ---- The lane, over the waterfall (F4OLI's redesign): each call shown on the tiles over its offset, on two lines
 * where they would meet; red the stations calling us, white the CQs, grey the others, yellow the tile selected and
 * our TX. The MFK and the knob leave it alone ---- */

static lv_obj_t *lane;
static lv_obj_t *board;                 /* the tiles, over the hidden list */
static bool      board_on;              /* View: the tiles show, else the traffic (the list); kept while the GUI runs */
static char      board_sel_call[16];    /* the tile selected, "" none */
static lv_timer_t *heard_timer;         /* each second: the red of 3 minutes, the stations of 2 */

static lv_coord_t lane_x(float hz) {
    if (filter_high <= filter_low) return 0;
    return (lv_coord_t)((hz - filter_low) * WIDTH / (filter_high - filter_low));
}

static void lane_update(void) {
    if (lane) lv_obj_invalidate(lane);
}

static int by_hz(const void *a, const void *b) {
    float x = (*(const ft8_heard_t *const *)a)->cd.meta.freq_hz, y = (*(const ft8_heard_t *const *)b)->cd.meta.freq_hz;

    return (x > y) - (x < y);
}

static void lane_draw_cb(lv_event_t *e) {
    static const ft8_heard_t *st[FT8_HEARD_MAX];
    lv_draw_ctx_t            *ctx  = lv_event_get_draw_ctx(e);
    uint32_t                  now  = lv_tick_get();
    int                       n    = ft8_heard_list(st, FT8_HEARD_MAX, param_i_get(cfg_ft8_show_all), now);
    lv_coord_t                ends[2] = {-1000, -1000};
    lv_draw_label_dsc_t       ld;
    lv_area_t                 box;

    lv_obj_get_coords(lane, &box);
    lv_draw_label_dsc_init(&ld);
    ld.font = &sony_16;
    qsort(st, n, sizeof(st[0]), by_hz);

    /* TX first: the calls give way to it on its line */
    for (int k = -1; k < n; k++) {
        const char *call = k < 0 ? "TX" : st[k]->cd.meta.call_de;
        lv_coord_t  x    = box.x1 + lane_x(k < 0 ? params.ft8_tx_freq.x : st[k]->cd.meta.freq_hz);
        lv_point_t  size;

        lv_txt_get_size(&size, call, ld.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (x + size.x > box.x2) x = box.x2 - size.x;

        int line = x > ends[0] + 6 ? 0 : x > ends[1] + 6 ? 1 : -1;

        if (line < 0) continue;     /* no room on either line: its tile has it */
        ends[line] = x + size.x;
        if (k < 0 || (board_on && !strcmp(call, board_sel_call))) ld.color = TC(warn);
        else if (ft8_heard_calls_you(st[k], now)) ld.color = TC(err);
        else if (st[k]->cd.cell_type == CELL_RX_CQ) ld.color = TC(text);
        else ld.color = TC(text_mute);

        lv_area_t a = {x, box.y1 + 1 + line * 12, x + size.x, box.y1 + 1 + line * 12 + 14};

        lv_draw_label(ctx, &ld, &a, call, NULL);
    }
}

static void build_lane(void) {
    lv_obj_t *l = lane = lv_obj_create(dialog.obj);

    lv_obj_remove_style_all(l);
    lv_obj_set_style_bg_color(l, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(l, WIDTH, LANE_H);
    lv_obj_set_pos(l, 13, 13);
    lv_obj_add_event_cb(l, lane_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
}

/* ---- The tiles (View: Tiles, F4OLI's redesign, as JS8's board): one a station heard in the last 2 minutes, in
 * ft8_heard_list's order (those calling us first), nine a page. A tile: flag, call, signal, country and distance, a
 * tag. The MFK goes from tile to tile, its press calls the station as a press on its last row does; a touch too.
 * The list stays under them, hidden, with the keys: decode_list.c hands them over (on_key, on_press) ---- */

#define BOARD_COLS  3
#define BOARD_ROWS  3
#define BOARD_N     (BOARD_COLS * BOARD_ROWS)
#define BARS        5

typedef struct {
    lv_obj_t *box, *flag, *call, *info, *tag, *bar[BARS];
} card_t;

static card_t             cards[BOARD_N];
static lv_obj_t          *board_note;       /* "Listening..." with no station, the page with more than nine */
static const ft8_heard_t *board_st[FT8_HEARD_MAX];
static int                board_n, board_sel, board_first;

static void card_show(card_t *c, const ft8_heard_t *st, bool sel) {
    const cell_data_t *cd    = &st->cd;
    uint32_t           now   = lv_tick_get();
    bool               fresh = ft8_heard_calls_you(st, now);

    lv_obj_clear_flag(c->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(c->box, fresh ? TC(row_to_me) : TC(row), LV_PART_MAIN);
    lv_obj_set_style_border_opa(c->box, sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);

    const lv_img_dsc_t *f = table_view_flag(cd->flag);

    if (f) {
        lv_img_set_src(c->flag, f);
        lv_obj_clear_flag(c->flag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c->flag, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(c->call, cd->meta.call_de);

    char info[48] = "";

    if (cd->country[0]) snprintf(info, sizeof(info), "%s", cd->country);
    if (cd->dist > 0) {
        bool miles = decode_list_miles();

        snprintf(info + strlen(info), sizeof(info) - strlen(info), "%s%d %s", info[0] ? "  " : "",
                 miles ? (int)(cd->dist * 0.621371f + 0.5f) : cd->dist, miles ? "mi" : "km");
    }
    lv_label_set_text(c->info, info);
    lv_obj_set_style_text_color(c->info, fresh ? TC(text) : TC(text_mute), LV_PART_MAIN);  /* grey on red hardly reads */

    /* their signal as we hear it, -24 dB one bar to -4 dB five */
    int lit = (cd->meta.local_snr + 29) / 5;

    for (int b = 0; b < BARS; b++) lv_obj_set_style_bg_color(c->bar[b], b < lit ? TC(ok) : TC(line), LV_PART_MAIN);

    char       tag[32];
    lv_color_t col = TC(text_mute);

    switch (ft8_heard_tag(st, now)) {
        case FT8_TAG_CALLS_YOU:     /* in the text's colour: the tile is red already */
            if (st->has_report) snprintf(tag, sizeof(tag), _("calls you %+03d"), st->report);
            else snprintf(tag, sizeof(tag), "%s", _("calls you"));
            col = TC(text);
            break;
        case FT8_TAG_NEW:
            snprintf(tag, sizeof(tag), "%s", _("NEW"));
            col = TC(warn);
            break;
        case FT8_TAG_CQ:
            snprintf(tag, sizeof(tag), "%s", cd->pota ? _("CQ POTA") : _("CQ"));
            col = TC(ok);
            break;
        case FT8_TAG_WORKED:     snprintf(tag, sizeof(tag), "%s", _("worked")); break;
        case FT8_TAG_CALLED_YOU: snprintf(tag, sizeof(tag), "%s", _("called you")); break;
        default:                 snprintf(tag, sizeof(tag), "%s", _("QSO")); break;
    }
    lv_label_set_text(c->tag, tag);
    lv_obj_set_style_text_color(c->tag, col, LV_PART_MAIN);
}

static void board_update(void) {
    if (!board || !board_on) return;
    board_n = ft8_heard_list(board_st, FT8_HEARD_MAX, param_i_get(cfg_ft8_show_all), lv_tick_get());

    /* the tile selected follows its station when the order changes */
    board_sel = -1;
    for (int k = 0; k < board_n; k++)
        if (board_sel_call[0] && !strcmp(board_st[k]->cd.meta.call_de, board_sel_call)) board_sel = k;
    if (board_sel >= 0) board_first = board_sel / BOARD_N * BOARD_N;
    if (board_first >= board_n) board_first = 0;

    for (int i = 0; i < BOARD_N; i++) {
        int k = board_first + i;

        if (k < board_n) card_show(&cards[i], board_st[k], k == board_sel);
        else lv_obj_add_flag(cards[i].box, LV_OBJ_FLAG_HIDDEN);
    }
    if (!board_n) {
        lv_label_set_text(board_note, "Listening... the stations heard come here");
        lv_obj_align(board_note, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(board_note, LV_OBJ_FLAG_HIDDEN);
    } else if (board_n > BOARD_N) {
        lv_label_set_text_fmt(board_note, "%d/%d", board_first / BOARD_N + 1, (board_n + BOARD_N - 1) / BOARD_N);
        lv_obj_align(board_note, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(board_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(board_note, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Speech: a tile as said, "Calling you, K 1 A B C, CQ, minus 12 dB, United States, 1200 km, new country, 3 of 9" */
static void tile_say(int k) {
    const ft8_heard_t *st = board_st[k];
    const cell_data_t *cd = &st->cd;
    char               call[48], what[64] = "", where[80] = "";

    if (!voice_auto_active()) return;
    voice_spell_calls(cd->meta.call_de, call, sizeof(call));
    if (ft8_heard_calls_you(st, lv_tick_get())) snprintf(what, sizeof(what), "Calling you, ");
    if (cd->country[0] && cd->dist > 0) snprintf(where, sizeof(where), ", %s, %i km", cd->country, cd->dist);
    else if (cd->country[0]) snprintf(where, sizeof(where), ", %s", cd->country);
    voice_delay_say_text_fmt("%s%s%s, %i d B%s%s%s, %i of %i", what, call,
                             cd->cell_type == CELL_RX_CQ ? ", C Q" : "", cd->meta.local_snr, where,
                             cd->new_dxcc ? ", new country" : "",
                             cd->cell_type == CELL_RX_CQ && cd->worked_type == SEARCH_WORKED_SAME_MODE ? ", worked" : "",
                             k + 1, board_n);
}

/* The MFK on the tiles: the next one, the tiles' page following it; its station's offset marked on the waterfall */
static void board_move(int step) {
    if (!board_n) {
        if (voice_auto_active()) voice_say_text_fmt("No stations yet");
        return;
    }

    int k = board_sel >= 0 ? (board_sel + step + board_n) % board_n : 0;

    snprintf(board_sel_call, sizeof(board_sel_call), "%s", board_st[k]->cd.meta.call_de);
    lv_finder_set_cursor(finder, board_st[k]->cd.meta.freq_hz);
    board_update();
    lane_update();
    tile_say(board_sel >= 0 ? board_sel : k);     /* board_update() sorted the tiles again: the one chosen by its call */
}

static void board_call(int k) {
    if (k < 0 || k >= board_n) return;

    cell_data_t cd = board_st[k]->cd;   /* a copy: the call brings new rows, and the tiles move */

    list_pressed(&cd);
}

static bool board_key(uint32_t key) {
    if (!board_on) return false;
    if (key == LV_KEY_LEFT || key == LV_KEY_UP) board_move(-1);
    else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) board_move(1);
    else return false;
    return true;
}

/* The first press with no tile selected picks the first; the next calls it. While we send, a press stops TX as on
 * the list (list_pressed) */
static bool board_press(void) {
    if (!board_on) return false;
    if (board_sel < 0 && !sending) board_move(0);
    else if (board_sel < 0) list_pressed(NULL);
    else board_call(board_sel);
    return true;
}

static void card_clicked_cb(lv_event_t *e) {
    int k = board_first + (int)(intptr_t)lv_event_get_user_data(e);

    if (k >= board_n) return;
    snprintf(board_sel_call, sizeof(board_sel_call), "%s", board_st[k]->cd.meta.call_de);
    board_update();
    lane_update();
    board_call(board_sel >= 0 ? board_sel : k);
}

static void fade_stop(void);

static void board_show(bool on) {
    board_on = on;
    if (!board) return;
    fade_stop();    /* the view that was fading out shows again in full when it comes back */
    if (on) {
        lv_obj_clear_flag(board, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(table_view_obj(), LV_OBJ_FLAG_HIDDEN);
        board_update();
    } else {
        lv_obj_add_flag(board, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(table_view_obj(), LV_OBJ_FLAG_HIDDEN);
    }
    lane_update();
}

static void build_board(void) {
    lv_obj_t *b = board = lv_obj_create(dialog.obj);

    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, WIDTH, LIST_H);
    lv_obj_set_pos(b, 13, LIST_Y);
    lv_obj_set_style_bg_color(b, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t cw = (WIDTH - (BOARD_COLS - 1) * 6) / BOARD_COLS;
    lv_coord_t ch = (LIST_H - 14 - (BOARD_ROWS - 1) * 4) / BOARD_ROWS;     /* the page's note under them */

    for (int i = 0; i < BOARD_N; i++) {
        card_t   *c   = &cards[i];
        lv_obj_t *box = c->box = lv_obj_create(b);

        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, cw, ch);
        lv_obj_set_pos(box, (i % BOARD_COLS) * (cw + 6), 2 + (i / BOARD_COLS) * (ch + 4));
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(box, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(box, TC(warn), LV_PART_MAIN);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(box, card_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        c->flag = lv_img_create(box);
        lv_img_set_zoom(c->flag, 384);  /* 24 x 18 */
        lv_obj_set_pos(c->flag, 12, 8);

        c->call = lv_label_create(box);
        lv_obj_set_style_text_font(c->call, &sony_22, LV_PART_MAIN);
        lv_obj_set_style_text_color(c->call, TC(text), LV_PART_MAIN);
        lv_obj_set_pos(c->call, 40, 2);

        for (int k = 0; k < BARS; k++) {
            lv_obj_t *bar = c->bar[k] = lv_obj_create(box);

            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, 4, 4 + k * 3);
            lv_obj_set_pos(bar, cw - 44 + k * 6, 22 - k * 3);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        }

        c->info = lv_label_create(box);
        lv_label_set_long_mode(c->info, LV_LABEL_LONG_DOT);
        lv_obj_set_width(c->info, cw - 120);
        lv_obj_set_style_text_font(c->info, &sony_16, LV_PART_MAIN);
        lv_obj_set_pos(c->info, 8, ch - 24);

        /* the tag on a box of the panel's colour: black ones hardly read in the light theme */
        c->tag = lv_label_create(box);
        lv_obj_set_style_text_font(c->tag, &sony_16, LV_PART_MAIN);
        lv_obj_set_style_bg_color(c->tag, TC(panel), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(c->tag, LV_OPA_60, LV_PART_MAIN);
        lv_obj_set_style_radius(c->tag, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(c->tag, 6, LV_PART_MAIN);
        lv_obj_align(c->tag, LV_ALIGN_BOTTOM_RIGHT, -6, -3);
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    }

    board_note = lv_label_create(b);
    lv_obj_set_style_text_font(board_note, &sony_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(board_note, TC(text_mute), LV_PART_MAIN);

    board_n           = 0;
    board_sel         = -1;
    board_first       = 0;
    board_sel_call[0] = '\0';
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
}

/* Each second: a red tile goes plain 3 minutes after its call, a station 2 minutes after its last row, with no new
 * decode to tell */
static void heard_timer_cb(lv_timer_t *t) {
    (void)t;
    board_update();
    lane_update();
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

/* The station in QSO: the first word of the next transmission ("DXCALL MYCALL ...", "<DXCALL> MYCALL RR73" with a
 * non-standard call of ours), none for a CQ ("CQ_POTA" too) */
static void udp_dx_call(char dx[16]) {
    qso_lock();
    const char *m = tx_msg.msg + (tx_msg.msg[0] == '<');

    snprintf(dx, 16, "%.*s", (int)strcspn(m, " >"), m);
    qso_unlock();
    if (!strcmp(dx, "CQ") || !strncmp(dx, "CQ_", 3)) dx[0] = '\0';
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
    bool     aside;     /* a QSO the sequence had put aside: the cursor is the current one's */
    int64_t  dial;      /* the dial and the mode its decodes were of */
    int      proto;
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
    worked_add_call(j->rec.remote_call, NULL);  /* its country is worked now: no NEW for it */
    ft8_heard_logged(j->rec.remote_call);       /* its tile no longer red */
    board_update();
    lane_update();
    ui_apply(&j->ui);
}

/* A QSO the sequence completed (the decoder thread, or the Save key on the GUI), after its step:
 * the ADIF and the log database here, the Field Ops entry and the cursor on the GUI thread */
static void save_qso(const done_qso_t *d) {
    const char *call = d->call, *grid = d->grid;
    const int   rcvd = d->rcvd, sent = d->sent;
    time_t now = time(NULL);
    int64_t dial = d->dial;
    char  *canon = util_canonize_callsign(call, false);

    qso_log_record_t rec = qso_log_record_create(params.callsign.x, canon, now, proto_of(d->proto)->log_mode, sent,
                                                 rcvd, dial, NULL, NULL, station_qth(), grid);
    free(canon);

    pthread_mutex_lock(&save_mux);
    adif_add_qso(ft8_log, rec);
    qso_log_record_save(rec);
    snprintf(last_logged, sizeof(last_logged), "%s", call);
    last_logged_at = now;
    pthread_mutex_unlock(&save_mux);

    saved_job_t job = {.rec = rec, .ui = {.cursor_off = !d->aside, .tx_gen = d->tx_gen, .win_gen = win_gen}};

    gui_post(save_qso_apply, &job, sizeof(job));
    ft8_udp_log_qso(now, rec.remote_call, grid, (uint64_t)dial, ftx_protocol_name(d->proto), sent, rcvd,
                    (int)param_f_get(cfg_pwr), params.callsign.x, station_qth());

    if (strlen(grid) >= 4) {
        double lat, lon;
        qth_str_to_pos(grid, &lat, &lon);
        double km = qth_pos_dist(lat, lon, my_lat, my_lon);
        bool   mi = decode_list_miles();    /* Settings > FT8/FT4 info, as the list (#227) */

        msg_schedule_long_text_fmt("Saved QSO de %s %d %d (%.0f %s)", call, sent, rcvd, mi ? km * 0.621371 : km,
                                   mi ? "mi" : "km");
    } else {
        msg_schedule_long_text_fmt("Saved QSO de %s %d %d", call, sent, rcvd);
    }
}

/* The QSOs completed in a step under qso_mux, for qso_unlock(): the processor's, in Hound mode
 * only one it had put aside (add_rx_text drops the others there), and Hound's own */
static done_qso_t done_qsos[2];
static int        done_n;

/* The processor's save callback, and Hound's RR73: under qso_mux, so noted only. The candidate
 * that called it may be gone once the lock is free */
static void qso_completed(const char *call, const char *grid, const int rcvd, const int sent, bool aside) {
    if (done_n == (int)ARRAY_SIZE(done_qsos)) {
        LV_LOG_ERROR("FT8: a QSO with %s completed, no room to save it", call);
        return;
    }
    done_qso_t *d = &done_qsos[done_n++];

    snprintf(d->call, sizeof(d->call), "%s", call);
    snprintf(d->grid, sizeof(d->grid), "%s", grid);
    d->rcvd   = rcvd;
    d->sent   = sent;
    d->aside  = aside;
    d->dial   = rx_held ? rx_dial : cparam_i_get(cfg_fg_freq);
    d->proto  = rx_held ? rx_proto : cur_proto();
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
    /* A call like KF0QGT/6Y answers in the type 4 form, with the hash of it kept for the calls to us */
    ftx_qso_processor_set_local_standard(p, !params.callsign.x[0] || ftx_call_is_standard(params.callsign.x));
    ftx_worker_set_own_call(params.callsign.x);
    ftx_qso_processor_set_hound(p, hound_on);                     /* a restart in Hound mode stays in it */
    qso_lock();     /* the panel's status reads it */
    qso        = p;
    decoder_on = true;
    rx_held    = false;
    qso_unlock();

    audio_worker_cb_t cb = {
        .on_message  = on_message,
        .on_psd      = on_psd,
        .on_slot_end = on_slot_end,
        .on_tick     = on_tick,
    };

    dec_proto    = cur_proto();
    audio_worker = audio_worker_create(SAMPLE_RATE, dec_proto, filter_low, filter_high, &cb);
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

/* The map has the keys while open: VOL (the knob, and the browser's, which comes the same way) passed on as the
 * list does, or it did nothing until the map closed (#305) */
static void map_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC) map_close();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT) radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT) radio_change_vol(1);
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

/* Empty list, tiles and waterfall, the MFK on the "Wait sync" row */
static void clear_view(void) {
    static uint32_t up = LV_KEY_UP;

    table_view_reset();
    ft8_heard_clear();
    board_sel_call[0] = '\0';
    board_update();
    lane_update();
    lv_waterfall_clear_data(waterfall);
    lv_event_send(table_view_obj(), LV_EVENT_KEY, &up);
}

/* Before a change of dial frequency or mode, which restarts the decoder (on), or none after all
 * (off): until it restarts, the old decoder's QSOs are logged with the dial and mode of before */
static void rx_hold(bool on) {
    int64_t dial = cparam_i_get(cfg_fg_freq);

    qso_lock();
    rx_held  = on;
    rx_dial  = dial;
    rx_proto = cur_proto();
    qso_unlock();
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
    rx_hold(true);
    if (!load_band(lv_event_get_code(e) == EVENT_BAND_UP ? 1 : -1)) {
        rx_hold(false);
        return;
    }
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
    lane_update();      /* its TX */
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

/* The view in sight fades: the list or the tiles */
static lv_obj_t *view_obj(void) {
    return board_on && board ? board : table_view_obj();
}

static void fade_to(lv_opa_t opa) {
    lv_anim_set_var(&fade, view_obj());
    lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(view_obj(), 0), opa);
    lv_anim_start(&fade);
}

/* No fade: both views in full (View changes while the knob turns) */
static void fade_stop(void) {
    lv_obj_t *views[2] = {table_view_obj(), board};

    for (int i = 0; i < 2; i++) {
        if (!views[i]) continue;
        lv_anim_del(views[i], fade_exec_cb);
        lv_obj_set_style_opa_layered(views[i], LV_OPA_COVER, 0);
    }
    fading = false;
    if (fade_back_timer) {
        lv_timer_del(fade_back_timer);
        fade_back_timer = NULL;
    }
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
    board_update();     /* Show: CQ on the tiles and the lane at once: the stations seen in a QSO go */
    lane_update();
}

static void foryou_cb(button_data_t *btn);

/* Show held: the "For you" strip on or off, as For you (page 5) does (F4OLI's shortcut) */
static void show_hold_cb(button_data_t *btn) {
    foryou_cb(btn);
    buttons_refresh(&button_foryou);
    msg_schedule_text_fmt(param_i_get(cfg_ft8_foryou) ? "For you strip: on" : "For you strip: off");
}

/* View: the traffic (the list) or the tiles */
static const char *view_label(void) {
    return board_on ? "View:\nTiles" : "View:\nTraffic";
}

static void view_cb(button_data_t *btn) {
    if (kb_open) return;
    board_show(!board_on);
    buttons_refresh(btn);
    if (voice_auto_active()) {
        if (board_on)
            voice_say_text_fmt("Tiles, %i station%s. M F K: station by station, press: call it", board_n,
                               board_n == 1 ? "" : "s");
        else
            voice_say_text_fmt("Traffic, the decode list");
    }
}

/* FT8 -> FT4 -> FT2 -> FT8 */
static void mode_cb(button_data_t *btn) {
    if (kb_open) return;

    int next = cur_proto() == FTX_PROTOCOL_FT8 ? FTX_PROTOCOL_FT4
             : cur_proto() == FTX_PROTOCOL_FT4 ? FTX_PROTOCOL_FT2 : FTX_PROTOCOL_FT8;

    rx_hold(true);
    param_i_set(cfg_ft8_protocol, next);
    freq_strip_set_app(ftx_protocol_name(next));
    decoder_restart();      /* a CQ ends with no note: TX CQ is next to Mode */
    if (params.ft8_custom_on.x) params_bool_set(&params.ft8_custom_on, false); /* FT4/FT2 elsewhere */
    if (next != FTX_PROTOCOL_FT8) {
        qso_lock();
        hound_on = false;
        ftx_qso_processor_set_hound(qso, false);
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

    char cq[sizeof(tx_msg.msg)];

    /* Made before CQ goes on: one that does not encode was only logged, and nothing went out each slot */
    if (!cq_make_message(params.callsign.x, station_qth(), params.ft8_cq_modifier.x, cq)) {
        msg_schedule_text_fmt("No CQ: callsign %s does not fit %s", params.callsign.x, mode_name());
        return;
    }
    /* "CQ PA/ON6URE": a call that is not a base call leaves no room for the QTH and the modifier set */
    bool short_cq = (params.ft8_cq_modifier.x[0] || station_qth()[0]) && !ftx_call_is_standard(params.callsign.x);

    subject_i_set(cq_enabled, true);
    subject_i_set(tx_enabled, true);
    qso_lock();
    strcpy(tx_msg.msg, cq);
    tx_odd = first_tx_slot_odd();
    /* "CQ_DX ON6URE JO21" goes out as "CQ DX ..." */
    msg_schedule_text_fmt("Next TX: %s%s%s", tx_msg.msg[2] == '_' ? "CQ " : "",
                          tx_msg.msg + (tx_msg.msg[2] == '_' ? 3 : 0),
                          short_cq ? " (no QTH or modifier with this call)" : "");
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
 * it is left alone (clock_sync.h's rule). The step is taken from the time to the nanosecond
 * (FT4's and FT2's slots start on fractions of a second, whole seconds missed them), and only a
 * clock that moved goes into the RTC */
#define SYNC_ON_SLOT_NS 50000000

static void time_sync_cb(button_data_t *btn) {
    struct timespec now;
    int             err_ms;
    int64_t         off;

    switch (clock_sync_keeper(&err_ms)) {
        case CLOCK_BY_NTP:
            msg_update_text_fmt("Clock in sync (NTP, within %d ms): no Time Sync needed", err_ms);
            return;
        case CLOCK_BY_GPS:
            msg_update_text_fmt("The GPS keeps the clock: no Time Sync needed");
            return;
        default:
            break;
    }
    clock_gettime(CLOCK_REALTIME, &now);
    off = ftx_slot_offset_ns(now.tv_sec, now.tv_nsec, ftx_slot_time(cur_proto()));
    if (llabs(off) < SYNC_ON_SLOT_NS) {
        msg_update_text_fmt("Clock on the slot already (%+.2f s)", off / 1e9);
        return;
    }
    if (!clock_sync_step(-off / 1e9, CLOCK_BY_HAND, "Time Sync")) {
        msg_update_text_fmt("Time Sync: the clock can't be set (%s)", strerror(errno));
        return;
    }
    msg_update_text_fmt("Clock moved %+.2f s to the slot start", -off / 1e9);
}

/* ---- Auto Sync (ft8/time_sync.h): the clock from the decodes' DT. The decodes of FT8 and FT4 are gathered as they
 * come (on_message), and once a slot, just past its middle (on_tick), what they say is looked at and told to the
 * GPS / Clock page (clock_sync.h), Auto Sync on or off. The clock moves only by clock_sync.h's rule: nothing keeps it
 * (neither NTP nor the GPS), Auto Sync is on (its switch is on the GPS / Clock page) and no time station set it in
 * the last hour; with enough stations agreeing that it is 0.2 s off or more, it moves then, unless something
 * transmits. All on the decoder thread: a transmission of the app holds it, so none is under way as the clock moves.
 * With NTP keeping the clock, the median DT a right clock shows here is learned (clock_sync_ft8_rx_dt) */

#define SYNC_LOG_MS 60000       /* the log line and the learning at most once a minute, a move always */

static ftx_sync_t  sync_state;
static int64_t     sync_slot_seen;      /* the slot last looked at (its number), decoder thread */
static int64_t     sync_logged_ms;
static bool        sync_said_far;       /* "too far" said: again only after a move */
static bool        sync_said_fail;

static int64_t clock_ns(clockid_t id) {
    struct timespec t;

    clock_gettime(id, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* A decode of a known sender (decoder thread): gathered always, the page shows what they say */
static void auto_sync_heard(const slot_info_t *slot, const char *call, float time_sec) {
    int64_t now = clock_ns(CLOCK_REALTIME), mono = clock_ns(CLOCK_MONOTONIC);

    ftx_sync_add(&sync_state, dec_proto, slot->rx_start_ns, slot->rx_start_mono_ns, time_sec, call, now, mono);
}

/* True once a minute (always: a move) */
static bool auto_sync_log(const ftx_sync_result_t *r, const char *what, bool always) {
    uint64_t ms = get_time();

    if (!always && sync_logged_ms && ms - sync_logged_ms < SYNC_LOG_MS) return false;
    sync_logged_ms = ms;
    fprintf(stderr, "ft8: Auto Time Sync: median DT %+.3f s (%+.3f s on time) of %d stations, %d slots, %d decodes, "
            "spread %.3f s: %s\n", r->median_dt, ftx_sync_rx_dt(), r->stations, r->slots, r->decodes, r->spread, what);
    return true;
}

/* Each tick of the decoder that sent nothing (decoder thread), sec into the slot */
static void auto_sync_tick(float sec) {
    int proto = dec_proto;

    if (!ftx_sync_proto(proto)) {
        if (sync_state.n) ftx_sync_reset(&sync_state);
        sync_said_far  = false;
        sync_said_fail = false;
        return;
    }

    float   slot = ftx_slot_time(proto);
    int64_t now  = clock_ns(CLOCK_REALTIME), mono = clock_ns(CLOCK_MONOTONIC);
    int64_t into = ftx_slot_offset_ns(now / 1000000000LL, (long)(now % 1000000000LL), slot);
    int64_t id   = (now - into) / llround(slot * 1e9);     /* the slot's number: its nearest start's */

    if (sec < slot / 2 || id == sync_slot_seen) return;
    sync_slot_seen = id;

    ftx_sync_result_t r;
    clock_by_t        keeper;

    clock_sync_ft8_rx_dt(NULL);     /* the learned DT into the checks (read from the card once) */

    ftx_sync_rc_t rc      = ftx_sync_check(&sync_state, proto, now, mono, &r);
    bool          allowed = clock_sync_allowed(CLOCK_BY_FT8, &keeper);

    clock_sync_ft8_word(rc, r.step, r.median_dt, r.stations, r.slots);
    if (rc == FTX_SYNC_WAIT) return;
    switch (keeper) {
        case CLOCK_BY_NTP:
            /* a right clock: what it reads by the decodes is learned, the stations agreeing */
            if (auto_sync_log(&r, "NTP keeps the clock", false) && rc != FTX_SYNC_SPREAD)
                clock_sync_ft8_rx_dt_learn(r.median_dt);
            return;
        case CLOCK_BY_GPS:
            auto_sync_log(&r, "the GPS keeps the clock", false);
            return;
        default:
            break;
    }
    if (!allowed) {
        auto_sync_log(&r, param_i_get(cfg_ft8_auto_sync) ? "a time station set the clock, measured only"
                                                         : "Auto Sync off, measured only", false);
        return;
    }
    switch (rc) {
        case FTX_SYNC_SPREAD:
            auto_sync_log(&r, "the stations disagree, no move", false);
            break;
        case FTX_SYNC_ON_TIME:
            auto_sync_log(&r, "on time", false);
            break;
        case FTX_SYNC_TOO_FAR:
            auto_sync_log(&r, "too far to move", false);
            if (!sync_said_far)
                msg_schedule_text_fmt("Auto Sync: by the decodes the clock is %.1f s %s, too far: press Time Sync as "
                                      "a slot starts", fabsf(r.step), r.step < 0 ? _("ahead") : _("behind"));
            sync_said_far = true;
            break;
        case FTX_SYNC_STEP:
            /* Nothing on the air (TUNE, the hand mic, a remote PTT too), and the moment: just past the slot's middle,
             * the clock staying inside the slot. Missed, the next slot looks again */
            if (sending || radio_get_state() == RADIO_TX || !ftx_sync_moment(proto, sec, r.step)) {
                auto_sync_log(&r, "not now", false);
                break;
            }
            {
                char what[32];

                snprintf(what, sizeof(what), _("%d stations"), r.stations);   /* shown on GPS / Clock */
                if (!clock_sync_step(r.step, CLOCK_BY_FT8, what)) {
                    if (!sync_said_fail)
                        msg_schedule_text_fmt("Auto Sync: the clock can't be set (%s)", strerror(errno));
                    sync_said_fail = true;
                    break;
                }
                ftx_sync_reset(&sync_state);    /* the DTs gathered were of the clock before */
                sync_said_far = false;
                snprintf(what, sizeof(what), "moved %+.3f s", r.step);
                auto_sync_log(&r, what, true);
            }
            msg_schedule_text_fmt("Auto Sync: clock moved %+.2f s by the decodes of %d stations", r.step, r.stations);
            break;
        case FTX_SYNC_WAIT:
            break;
    }
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
    rx_hold(true);
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

/* DXpedition Hound: on or off, a QSO or a CQ in progress stops, a QSO with our report out is kept
 * aside for its station's late message */
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
    ftx_qso_processor_set_hound(qso, hound_on);
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

/* The labels show the state now; a press changes it. F4OLI's pages: operate, the QSO, the band, the tools; then
 * ours that their four left to a hold (Log Map, For you) as buttons of their own. Auto Sync: on the GPS / Clock page */
static button_data_t button_page_1  = {.type = BTN_TEXT, .label = "(FT8 1:5)", .press = button_next_page_cb, .next = &page_2};
static button_data_t button_tx_cq   = {.type = BTN_TEXT_FN, .label_fn = tx_cq_label, .press = tx_cq_cb};
static button_data_t button_tx_call = {.type = BTN_TEXT_FN, .label_fn = tx_call_label, .press = tx_call_cb};
static button_data_t button_show    = {.type = BTN_TEXT_FN, .label_fn = show_label, .press = show_cb, .hold = show_hold_cb};
static button_data_t button_view    = {.type = BTN_TEXT_FN, .label_fn = view_label, .press = view_cb};

static button_data_t button_page_2  = {.type = BTN_TEXT, .label = "(FT8 2:5)", .press = button_next_page_cb, .next = &page_3};
static button_data_t button_auto    = {.type = BTN_TEXT_FN, .label_fn = auto_label, .press = auto_cb};
static button_data_t button_hold    = {.type = BTN_TEXT_FN, .label_fn = hold_label, .press = hold_cb};
static button_data_t button_retries = {.type = BTN_TEXT_FN, .label_fn = retries_label, .press = retries_cb};
static button_data_t button_force   = {.type = BTN_TEXT, .label = "Force QSO\nsave", .press = force_save_cb};

static button_data_t button_page_3  = {.type = BTN_TEXT, .label = "(FT8 3:5)", .press = button_next_page_cb, .next = &page_4};
static button_data_t button_mode    = {.type = BTN_TEXT_FN, .label_fn = mode_label, .press = mode_cb};
static button_data_t button_freq    = {.type = BTN_TEXT_FN, .label_fn = freq_label, .press = freq_cb, .hold = freq_hold_cb};
static button_data_t button_hound   = {.type = BTN_TEXT_FN, .label_fn = hound_label, .press = hound_cb};

/* TUNE for an external tuner: a tone at the TX offset, where the app transmits (ext_tune.h) */
static button_data_t button_page_4  = {.type = BTN_TEXT, .label = "(FT8 4:5)", .press = button_next_page_cb, .next = &page_5};
static button_data_t button_cq_mod  = {.type = BTN_TEXT, .label = "CQ\nModifier", .press = cq_modifier_cb};
static button_data_t button_sync    = {.type = BTN_TEXT, .label = "Time\nSync", .press = time_sync_cb};
static button_data_t button_live_map = {.type = BTN_TEXT, .label = "Live\nMap", .press = heard_map_cb, .hold = map_cb};

static button_data_t button_page_5  = {.type = BTN_TEXT, .label = "(FT8 5:5)", .press = button_next_page_cb, .next = &page_1};
static button_data_t button_log_map = {.type = BTN_TEXT, .label = "Log\nMap", .press = map_cb};
static button_data_t button_foryou  = {.type = BTN_TEXT_FN, .label_fn = foryou_label, .press = foryou_cb};

static buttons_page_t page_1 = {{&button_page_1, &button_tx_cq, &button_tx_call, &button_show, &button_view}};
static buttons_page_t page_2 = {{&button_page_2, &button_auto, &button_hold, &button_retries, &button_force}};
static buttons_page_t page_3 = {{&button_page_3, &button_mode, &button_freq, &button_hound, &btn_dig_pwr}};
static buttons_page_t page_4 = {{&button_page_4, &button_cq_mod, &button_sync, &button_live_map, &btn_ext_tune}};
static buttons_page_t page_5 = {{&button_page_5, &button_log_map, &button_foryou}};

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
    rx_hold(true);
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
    if (!cd || cd->cell_type == CELL_TX_MSG || cd->cell_type == CELL_RX_INFO || cd->cell_type == CELL_START_QSO ||
        cd->cell_type == CELL_SLOT_SEP) {   /* a slot's line: no station (in Hound it said "Select the Fox's CQ") */
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

    /* Free text (Show: All), or a call we know only as "<...>" (qso.cpp): no station to answer. The answer made had no
     * call in it, " ON6URE JO21", and went out as free text for all its retries, the TX offset moved to it (#228) */
    if (!cd->meta.call_de[0]) {
        msg_schedule_text_fmt("No call to answer in that message");
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

/* The start of the slot we are in, UTC: the time of its rows and of our transmission in it. Read as the tick came,
 * it was 1 to 3 s late on a busy band (07:42:18, 07:42:46 on 40 m with 20 to 26 decodes a slot): the slot's last
 * decode runs before the tick (F4OLI) */
static void slot_start_utc(struct tm *utc) {
    struct timespec now;
    time_t          start;

    clock_gettime(CLOCK_REALTIME, &now);
    start = (time_t)ftx_slot_start(now.tv_sec, now.tv_nsec, ftx_slot_time(cur_proto()));
    gmtime_r(&start, utc);
}

/* Our transmission as a row, "12:00:15 DL1ABC ON6URE JO21": its slot's start, as the decodes */
static void add_tx_row(const char *msg) {
    cell_data_t row = {.cell_type = CELL_TX_MSG};
    char        text[sizeof(row.text)];
    struct tm   utc;

    snprintf(text, sizeof(text), "%s", msg);
    if (!strncmp(text, "CQ_", 3)) text[2] = ' ';
    slot_start_utc(&utc);
    strftime(row.text, 10, "%H:%M:%S ", &utc);
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
        qso_completed(hound.fox, hound.fox_grid, hound.rcvd, hound.sent, false);
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
static void add_rx_text(int16_t snr, const char *text, const slot_info_t *slot, float freq_hz, float time_sec) {
    bool           odd  = slot->odd;
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
        /* The processor follows the Fox too, its answers unsent, and may complete that QSO: Hound's
         * RR73 alone logs it. A QSO from before Hound on (aside set) is logged at its station's late
         * message, unless that station is the Fox */
        int kept = done;
        for (int i = done; i < done_n; i++)
            if (done_qsos[i].aside && strcmp(done_qsos[i].call, hound.fox)) done_qsos[kept++] = done_qsos[i];
        done_n = kept;
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

    /* The heard map, PSK Reporter and Auto Sync: the sender of any message we can parse. A CB station's free text
     * ("1AT106 -07") names no sender: the call is the QSO's it was taken for, no spot */
    if (parsed) {
        auto_sync_heard(slot, meta.call_de, time_sec);
        map_rx_t rx = {meta, odd};
        if (!meta.free_text) js8_map_heard(meta.call_de, meta.grid, snr, meta.to_me, &rx, sizeof(rx));
        if (param_i_get(cfg_pskreporter) && !meta.free_text) {
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
    add_rx_text((int16_t)snr, text, slot, freq_hz, time_sec);
}

typedef struct {
    float   *db;
    size_t   n;
    uint32_t win_gen;
} wf_row_t;

/* Into the waterfall of the window it was made for, if that is still open; freed either way */
static void wf_row_add(void *data) {
    wf_row_t *row = data;

    if (row->win_gen == win_gen) {
        lv_waterfall_add_data(waterfall, row->db, row->n);
        remote_server_app_wf(row->db, row->n, filter_low, filter_high);     /* the FT8 page's */
    }
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
 * taken under qso_mux and sent without it: the GUI may stop or change it meanwhile.
 * first: the slot's first look (audio_worker.c's new slot). Its sec is read after the decoder's last pass of the
 * slot before, which can run past the slot's start (a busy band, a busy radio). Past tx_start_limit() a message due
 * since then was dropped for the slot with no row and no log line, a skipped cycle, while the same lateness from the
 * pass after the read went to tx_worker.c. Now on the first look FT8 and FT4 always go there: what is past is left
 * out by the clock, and past a fifth nothing is keyed ("TX not sent: too late into the slot", the retry kept). FT2's
 * first look comes before its last decode and nothing after it checks the time: its limit stays (#228) */
static bool transmit(const slot_info_t *slot, float sec, bool first) {
    ftx_tx_msg_t out;
    ui_job_t     j = {0};
    uint32_t     gen;
    bool         overran = sec >= tx_start_limit();

    qso_lock();
    if (!decoder_on || (overran && (!first || is_ft2())) || !tx_msg.msg[0] || tx_odd != slot->odd ||
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
    if (!hound_on) ftx_qso_processor_sent(qso, out.msg);   /* our report out: its QSO may be kept aside */
    qso_unlock();
    if (overran)
        fprintf(stderr, "ft8: the slot's first look at %.2f s, the decoder's pass before it ran past the slot's start\n",
                sec);
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

    tx_worker_rc_t rc = tx_worker_run(out.msg, AUDIO_PLAY_RATE, base_gain_offset, tx_stopped_cb, NULL);

    /* It counts for the message that went out, not for one the operator chose meanwhile. One too late into its slot
     * to key (the clock stepped since the slot's tick, a long last decode) did not go: its repeat is kept */
    qso_lock();
    sending = false;
    j = (ui_job_t){.tx_gen = tx_gen};
    if (tx_gen == gen && rc != TX_WORKER_LATE) {
        if (tx_msg.repeats > 0) tx_msg.repeats--;
        if (tx_msg.repeats == 0) {      /* given up: a CQ ends with it */
            j.cq_off      = !strncmp(tx_msg.msg, "CQ", 2);
            tx_msg.msg[0] = '\0';
        }
    }
    qso_unlock();
    if (rc == TX_WORKER_LATE) {
        add_info("TX not sent: too late into the slot");
        return false;
    }
    if (j.cq_off) ui_post(&j);
    return true;
}

static void on_tick(const slot_info_t *slot, bool new_slot, float sec, void *ctx) {
    qso_lock();
    bool tx_pending = tx_msg.msg[0] != '\0';
    qso_unlock();

    if (transmit(slot, sec, new_slot)) return;
    if (!new_slot) {
        auto_sync_tick(sec);
        return;
    }

    static unsigned ft2_slots;
    struct tm       utc;

    sending = false;
    slot_start_utc(&utc);
    snprintf(slot_time, sizeof(slot_time), "%02i:%02i:%02i", utc.tm_hour, utc.tm_min, utc.tm_sec);

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
    lv_waterfall_set_size(waterfall, WIDTH, WF_HEIGHT - LANE_H);
    lv_waterfall_set_min(waterfall, 0);    /* above the noise floor (on_psd), as JS8 */
    lv_waterfall_set_max(waterfall, 30);
    lv_obj_set_pos(waterfall, 13, 13 + LANE_H);
    build_lane();

    finder = lv_finder_create(waterfall);
    lv_finder_set_width(finder, protos[FTX_PROTOCOL_FT8].width_hz);
    lv_finder_set_value(finder, params.ft8_tx_freq.x);
    freq_strip_set_offset(params.ft8_tx_freq.x);
    lv_obj_set_size(finder, WIDTH, WF_HEIGHT - LANE_H);
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
    table_view_build(dialog.obj, 13, LIST_Y, WIDTH, LIST_H);
    table_view_set_press_cb(list_pressed);
    table_view_set_actions(&(table_view_actions_t){.on_close = list_close, .on_vol_change = list_vol,
                                                   .on_key = board_key, .on_press = board_press});

    /* The tiles over the list (F4OLI's redesign), as View was left; the countries of the log for their NEW */
    ft8_heard_clear();
    ft8_worked_clear();
    qso_log_each_call(worked_add_call, NULL);
    build_board();
    board_show(board_on);
    heard_timer = lv_timer_create(heard_timer_cb, 1000, NULL);

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
    ftx_sync_reset(&sync_state);    /* the decoder is not running yet */
    sync_slot_seen = 0;

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
    lane      = NULL;
    board     = NULL;
    freq_list = NULL;
    if (heard_timer) {
        lv_timer_del(heard_timer);
        heard_timer = NULL;
    }
    if (fade_back_timer) {      /* its fade_to() would find the list gone */
        lv_timer_del(fade_back_timer);
        fade_back_timer = NULL;
    }
    fading = false;
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
