/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - JTTY window
 *
 *  JTTY is the keyboard mode of WSJT-X 3.2 (WSJT Development Group, K1JT
 *  et al.): 4-GFSK frames of 1.888 s, sent whenever there is something to
 *  say. The protocol is in jtty.c, the receiver in jtty_rx.c, the
 *  transmitter in jtty_tx.c; this file is the window, laid out as the PSK
 *  and Olivia ones: the waterfall on top, the received text in a panel
 *  under it, and the line being typed under that.
 *
 *  The receiver decodes the whole passband. Messages within the
 *  tolerance of the operator's frequency (the finder) print in the text
 *  colour; the others are dimmed, with their audio frequency in front.
 *  A message grows on its line frame by frame, as in WSJT-X.
 *
 *  Typing on a USB or Bluetooth keyboard goes into the TX line, Enter
 *  sends it; a line sent while another is on the air follows it without a
 *  gap. F1-F8 are macros, by default the WSJT-X JTTY function keys (CQ %M CQ, %H %E, ...),
 *  packed with the contest exchange forms, so "599 001" takes one frame.
 */

#include "dialog_jtty.h"
#include "board.h"

#include "lvgl/lvgl.h"

#include "dialog.h"
#include "styles.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "audio.h"
#include "dsp.h"
#include "events.h"
#include "buttons.h"
#include "main_screen.h"
#include "keyboard.h"
#include "msg.h"
#include "scheduler.h"
#include "textarea_window.h"
#include "theme.h"
#include "params/params.h"
#include "pskreporter.h"
#include "station.h"
#include "main.h"

#include "widgets/lv_waterfall.h"
#include "widgets/lv_finder.h"

#include "jtty.h"
#include "jtty_rx.h"
#include "jtty_tx.h"
#include "psk_spec.h"           /* the waterfall of the keyboard modes */
#include "digi_qso.h"           /* shared QSO state: DX call */

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define WIDTH           771
#define WF_HEIGHT       118
#define TEXT_Y          (13 + WF_HEIGHT + 6)
#define TEXT_PAD        6
#define GAP             6

/* Finder: the four tones, 0..94 Hz above the frequency, and the skirts */
#define FINDER_LO_HZ    16
#define FINDER_W_HZ     126

#define ROTARY_HZ       5
#define TRACK_PERIOD_MS 500
#define KNOB_HOLD_MS    8000

#define TXT_LINES       24
#define TXT_LINE_LEN    96
#define TXT_SHOW        10

#define DRAFT_LEN       JTTY_MSG_LEN

#define DIAL_STEP_HZ    1000
#define DIAL_MAX_HZ     50000

/* JTTY dial frequencies of WSJT-X 3.2 (models/FrequencyList.cpp; 14.090
 * MHz is the suggested meeting place), USB, the signals 200-2800 Hz above */
typedef struct {
    const char *label;
    uint32_t    dial_hz;
} jtty_band_t;

static const jtty_band_t bands[] = {
    {"160m", 1838000},  {"80m", 3575000},  {"40m", 7090000},  {"30m", 10140000}, {"20m", 14090000},
    {"17m", 18100000},  {"15m", 21090000}, {"12m", 24920000}, {"10m", 28090000},
};

#define BAND_CNT ((int)(sizeof(bands) / sizeof(bands[0])))

/* Rx window tolerance, Hz (WSJT-X: FTol, 20 by default) */
static const uint16_t tols[] = {10, 20, 50, 100};

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);
static void audio_cb(unsigned int n, float *samples);

static lv_obj_t   *waterfall = NULL;
static lv_obj_t   *finder = NULL;
static lv_obj_t   *text_box = NULL;
static lv_obj_t   *tx_box = NULL;
static lv_obj_t   *tx_lbl = NULL;
static lv_obj_t   *tx_state = NULL;
static lv_obj_t   *line_lbl[TXT_SHOW];
static const lv_font_t *txt_font = &sony_24;
static lv_coord_t  text_h = 0;

static lv_timer_t *status_timer = NULL;
static lv_timer_t *text_timer = NULL;
static lv_timer_t *focus_timer = NULL;
static lv_timer_t *knob_timer = NULL;

static bool        alive = false;
static int         cur_band = -1;
static int32_t     dial_offset_hz = 0;
static bool        knob_dial = false;
static bool        input_active = false;
static bool        was_tx = false;
static int         tol_idx = 1;
static int         serial = 1;
static char        draft[DRAFT_LEN + 1];
static bool        draft_dirty = false;

static pthread_mutex_t spec_mux = PTHREAD_MUTEX_INITIALIZER;
static psk_spec_t  spec;
static bool        spec_ok = false;
static float       sp_edge_lo = 200.0f;
static float       sp_edge_hi = 2800.0f;

/* F1-F8 are a macro list, as the PSK and Olivia ones: /mnt/jtty_macros.txt, one
 * "name<TAB>text" line per key in order. A missing file is written with the WSJT-X
 * keys; keys past the end of the list are empty. Read at every opening. */
#define JTTY_MACROS_PATH BOARD_DATA "/jtty_macros.txt"
#define FKEYS           8
#define FKEY_NAME       24

static char        fkey_name[FKEYS][FKEY_NAME];
static char        fkey_text[FKEYS][JTTY_MSG_LEN + 1];
static char        fkey_label[FKEYS][FKEY_NAME + 4];
static int         fkey_count = 0;
static int         edit_key = -1;
static char        edit_name[FKEY_NAME];
static lv_timer_t *editor_timer = NULL;

/* ---- Buttons ---------------------------------------------------------- */

static const char *status_label_getter(void);
static const char *dx_label_getter(void);
static const char *serial_label_getter(void);
static const char *dial_label_getter(void);
static const char *tol_label_getter(void);
static void text_press_cb(button_data_t *b);
static void stop_press_cb(button_data_t *b);
static void status_press_cb(button_data_t *b);
static void status_hold_cb(button_data_t *b);
static void dx_press_cb(button_data_t *b);
static void dx_hold_cb(button_data_t *b);
static void serial_press_cb(button_data_t *b);
static void serial_hold_cb(button_data_t *b);
static void dial_press_cb(button_data_t *b);
static void dial_hold_cb(button_data_t *b);
static void tol_press_cb(button_data_t *b);
static void clear_press_cb(button_data_t *b);
static void fkey(int n);

static buttons_page_t page_1, page_2, page_3, page_4, page_5;

static button_data_t btn_p1 = {
    .type = BTN_TEXT, .label = "(JTTY 1:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb, .next = &page_2, .prev = &page_5,
};
static button_data_t btn_text = {.type = BTN_TEXT, .label = "Free\nText", .press = text_press_cb};
static button_data_t btn_stop = {.type = BTN_TEXT, .label = "Stop\nTX", .press = stop_press_cb};
static button_data_t btn_dx = {
    .type = BTN_TEXT_FN, .label_fn = dx_label_getter, .press = dx_press_cb, .hold = dx_hold_cb,
};
static button_data_t btn_status = {
    .type = BTN_TEXT_FN, .label_fn = status_label_getter, .press = status_press_cb, .hold = status_hold_cb,
};

/* The function keys: press sends, as F1..F8 on a keyboard; hold edits */
static void fkey_hold(int n);
#define FKEY_BTN(n)                                                                              \
    static void fkey##n##_cb(button_data_t *b) { (void)b; fkey(n); }                             \
    static void fkey##n##_hold_cb(button_data_t *b) { (void)b; fkey_hold(n); }                   \
    static button_data_t btn_f##n = {                                                            \
        .type = BTN_TEXT, .label = fkey_label[n - 1], .press = fkey##n##_cb, .hold = fkey##n##_hold_cb};

FKEY_BTN(1)
FKEY_BTN(2)
FKEY_BTN(3)
FKEY_BTN(4)
FKEY_BTN(5)
FKEY_BTN(6)
FKEY_BTN(7)
FKEY_BTN(8)

#undef FKEY_BTN

static button_data_t *const fkey_btns[FKEYS] = {&btn_f1, &btn_f2, &btn_f3, &btn_f4,
                                                 &btn_f5, &btn_f6, &btn_f7, &btn_f8};

static button_data_t btn_p2 = {
    .type = BTN_TEXT, .label = "(JTTY 2:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb, .next = &page_3, .prev = &page_1,
};
static button_data_t btn_p3 = {
    .type = BTN_TEXT, .label = "(JTTY 3:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb, .next = &page_4, .prev = &page_2,
};
static button_data_t btn_p4 = {
    .type = BTN_TEXT, .label = "(JTTY 4:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb, .next = &page_5, .prev = &page_3,
};
/* TUNE for an external tuner, on the TX frequency chosen (ext_tune.h) */
static button_data_t btn_p5 = {
    .type = BTN_TEXT, .label = "(JTTY 5:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb, .next = &page_1, .prev = &page_4,
};
static button_data_t btn_serial = {
    .type = BTN_TEXT_FN, .label_fn = serial_label_getter, .press = serial_press_cb, .hold = serial_hold_cb,
};
static button_data_t btn_dial = {
    .type = BTN_TEXT_FN, .label_fn = dial_label_getter, .press = dial_press_cb, .hold = dial_hold_cb,
};
static button_data_t btn_tol = {.type = BTN_TEXT_FN, .label_fn = tol_label_getter, .press = tol_press_cb};
static button_data_t btn_clear = {.type = BTN_TEXT, .label = "Clear", .press = clear_press_cb};

static buttons_page_t page_1 = {{&btn_p1, &btn_text, &btn_stop, &btn_dx, &btn_status}};
static buttons_page_t page_2 = {{&btn_p2, &btn_f1, &btn_f2, &btn_f3, &btn_f4}};
static buttons_page_t page_3 = {{&btn_p3, &btn_f5, &btn_f6, &btn_f7, &btn_f8}};
static buttons_page_t page_4 = {{&btn_p4, &btn_serial, &btn_tol, &btn_dial, &btn_clear}};
static buttons_page_t page_5 = {{&btn_p5, &btn_ext_tune}};

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .audio_cb = audio_cb,
    .rotary_cb = rotary_cb,
    .key_cb = key_cb,
    .restore_radio = true,
};

dialog_t *dialog_jtty = &dialog;

static void refresh_btn(button_data_t *b)
{
    if (b && b->disp_btn)
        buttons_refresh(b);
}

/* ---- Text panel ------------------------------------------------------- */

typedef struct {
    char     s[TXT_LINE_LEN];
    uint8_t  kind;      /* 0 received, 1 received elsewhere (dim), 2 sent */
    uint32_t id;        /* receiver's message id, 0 = none */
} txt_line_t;

static txt_line_t txt[TXT_LINES];
static int        txt_head = -1;
static int        txt_cnt = 0;
static bool       txt_dirty = false;

static void txt_clear(void)
{
    memset(txt, 0, sizeof(txt));
    txt_head = -1;
    txt_cnt = 0;
    txt_dirty = true;
}

static txt_line_t *txt_add(uint8_t kind, uint32_t id)
{
    txt_head = (txt_head + 1) % TXT_LINES;
    if (txt_cnt < TXT_LINES)
        txt_cnt++;
    txt[txt_head].kind = kind;
    txt[txt_head].id = id;
    txt[txt_head].s[0] = '\0';
    txt_dirty = true;
    return &txt[txt_head];
}

static txt_line_t *txt_find(uint32_t id)
{
    for (int k = 0; k < txt_cnt; k++) {
        txt_line_t *l = &txt[(txt_head - k + TXT_LINES) % TXT_LINES];
        if (id && l->id == id)
            return l;
    }
    return NULL;
}

/* One label per line, stacked upwards from the bottom (as dialog_psk.c) */
static void txt_render(void)
{
    int n = txt_cnt < TXT_SHOW ? txt_cnt : TXT_SHOW;
    lv_coord_t y = text_h - 2 * TEXT_PAD - 2;

    for (int k = n; k < TXT_SHOW; k++)
        lv_obj_add_flag(line_lbl[k], LV_OBJ_FLAG_HIDDEN);
    for (int k = 0; k < n; k++) {
        const txt_line_t *l = &txt[(txt_head - k + TXT_LINES) % TXT_LINES];
        lv_obj_t *lbl = line_lbl[k];
        lv_point_t size;

        lv_txt_get_size(&size, l->s, txt_font, 0, 0, WIDTH - 2 * TEXT_PAD - 2, LV_TEXT_FLAG_NONE);
        y -= size.y;
        if (y < 0) {
            for (int j = k; j < n; j++)
                lv_obj_add_flag(line_lbl[j], LV_OBJ_FLAG_HIDDEN);
            break;
        }
        lv_label_set_text(lbl, l->s);
        lv_obj_set_style_text_color(lbl, l->kind == 2 ? TC(tx) : l->kind == 1 ? TC(text_dim) : TC(text), 0);
        lv_obj_set_pos(lbl, 0, y);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void draft_render(void)
{
    char buf[DRAFT_LEN + 8];
    int  fl = jtty_tx_frames_left();

    snprintf(buf, sizeof(buf), "%s_", draft);
    lv_label_set_text(tx_lbl, buf);
    if (fl > 0) {
        lv_label_set_text_fmt(tx_state, "TX %d", fl);
        lv_obj_set_style_text_color(tx_state, TC(tx), 0);
    } else {
        lv_label_set_text(tx_state, "");
    }
}

static void text_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (txt_dirty && text_box) {
        txt_dirty = false;
        txt_render();
    }
    if (draft_dirty && tx_lbl) {
        draft_dirty = false;
        draft_render();
    }
}

/* ---- Received messages ------------------------------------------------- */

static bool in_window(float f)
{
    return fabsf(f - (float)jtty_tx_get_freq()) < tols[tol_idx];
}

/* UI thread: a message, new or grown */
static void rx_msg_ui(void *data)
{
    const jtty_rx_msg_t *m = (const jtty_rx_msg_t *)data;
    txt_line_t *l;
    bool win;
    char before[16];

    if (!alive)
        return;
    l = txt_find(m->id);
    win = l ? l->kind == 0 : in_window(m->freq);
    if (!l)
        l = txt_add(win ? 0 : 1, m->id);
    if (win)
        snprintf(l->s, sizeof(l->s), "%s", m->text);
    else
        snprintf(l->s, sizeof(l->s), "%4.0f  %s", m->freq, m->text);
    txt_dirty = true;

    /* A CQ heard is a spot; the call is a field of its own, not text */
    if (m->complete && m->call[0] && (m->call_kind == JTTY_CALL_CQ || m->call_kind == JTTY_CALL_TU_CQ) &&
        param_i_get(cfg_pskreporter))
        pskr_spot(params.callsign.x, station_qth(), VERSION, m->call, "",
                  (uint32_t)cparam_i_get(cfg_fg_freq) + (uint32_t)lroundf(m->freq), m->snr, "JTTY");

    /* The DX call: a station calling CQ, or calling us, on our frequency */
    if (!win)
        return;
    snprintf(before, sizeof(before), "%s", digi_qso_dx());
    if (digi_qso_autograb() && m->call[0] && strcasecmp(m->call, params.callsign.x) != 0 &&
        (m->call_kind == JTTY_CALL_CQ || m->call_kind == JTTY_CALL_TU_CQ || m->call_kind == JTTY_CALL_CALL))
        digi_qso_set_dx(m->call);
    if (m->complete)
        for (const char *p = m->text; *p; p++)
            digi_qso_feed_rx(*p);
    if (strcmp(before, digi_qso_dx()) != 0)
        refresh_btn(&btn_dx);
}

/* Receiver thread */
static void rx_msg_cb(const jtty_rx_msg_t *m)
{
    scheduler_put(rx_msg_ui, (void *)m, sizeof(*m));
}

/* ---- Spectrum and waterfall ------------------------------------------- */

struct wf_line {
    float *db;
    int    n;
};

static void wf_line_ui(void *data)
{
    struct wf_line *l = (struct wf_line *)data;

    if (alive && waterfall)
        lv_waterfall_add_data(waterfall, l->db, (uint16_t)l->n);
    free(l->db);
}

/* Audio thread */
static void on_spec_line(const float *db, int nbins, void *ctx)
{
    struct wf_line l;

    (void)ctx;
    if (!db || nbins <= 0)
        return;
    l.n = nbins;
    l.db = (float *)malloc(sizeof(float) * (size_t)nbins);
    if (!l.db)
        return;
    memcpy(l.db, db, sizeof(float) * (size_t)nbins);
    /* A full queue refuses it: the line is lost, not its memory (audit C06) */
    if (!scheduler_put(wf_line_ui, &l, sizeof(l)))
        free(l.db);
}

static void update_finder(void)
{
    int f = (int)jtty_tx_get_freq();

    lv_finder_set_range(finder, (int16_t)lroundf(sp_edge_lo), (int16_t)lroundf(sp_edge_hi));
    lv_finder_set_width(finder, FINDER_W_HZ);
    lv_finder_set_value(finder, (int16_t)(f - FINDER_LO_HZ));
    lv_obj_invalidate(finder);
}

static void set_freq(int hz)
{
    int lo = (int)ceilf(sp_edge_lo) + FINDER_LO_HZ, hi = (int)floorf(sp_edge_hi) - (FINDER_W_HZ - FINDER_LO_HZ);

    if (hz < lo)
        hz = lo;
    if (hz > hi)
        hz = hi;
    jtty_tx_set_freq((uint16_t)hz);
    jtty_rx_set_freq((float)jtty_tx_get_freq(), tols[tol_idx]);
    update_finder();
    refresh_btn(&btn_status);
}

static void apply_range(void)
{
    float lo = (float)cparam_i_get(cfg_cur_filter_low);
    float hi = (float)cparam_i_get(cfg_cur_filter_high);

    if (lo < 100.0f)
        lo = 100.0f;
    if (hi > 3000.0f)
        hi = 3000.0f;
    if (hi - lo < 400.0f) {
        lo = 200.0f;
        hi = 2800.0f;
    }
    pthread_mutex_lock(&spec_mux);
    psk_spec_set_range(&spec, lo, hi);
    psk_spec_reset(&spec);
    sp_edge_lo = psk_spec_edge_lo_hz(&spec);
    sp_edge_hi = psk_spec_edge_hi_hz(&spec);
    pthread_mutex_unlock(&spec_mux);

    jtty_rx_set_range(sp_edge_lo, sp_edge_hi);
    set_freq((int)jtty_tx_get_freq());
}

/* ---- Band and dial ----------------------------------------------------- */

static void load_band(int8_t dir)
{
    int idx = 0;

    if (dir == 0 || cur_band < 0) {
        int64_t best = -1, f = cparam_i_get(cfg_fg_freq);
        for (int i = 0; i < BAND_CNT; i++) {
            int64_t d = llabs(f - (int64_t)bands[i].dial_hz);
            if (best < 0 || d < best) {
                best = d;
                idx = i;
            }
        }
    } else {
        idx = (cur_band + dir + BAND_CNT) % BAND_CNT;
    }
    cur_band = idx;
    dial_offset_hz = 0;
    refresh_btn(&btn_dial);
    cparam_i_set(cfg_fg_freq, (int32_t)bands[idx].dial_hz);
    cparam_i_set(cfg_cur_mode, radio_mode_usb_dig);
    msg_update_text_fmt("JTTY %s  %u.%03u MHz", bands[idx].label, (unsigned)(bands[idx].dial_hz / 1000000u),
                        (unsigned)((bands[idx].dial_hz / 1000u) % 1000u));
}

static void band_cb(lv_event_t *e)
{
    if (input_active)
        return;
    if (jtty_tx_is_on()) {
        msg_update_text_fmt("#%06x JTTY: stop TX first#", theme_hex(TC_err));
        return;
    }
    load_band(lv_event_get_code(e) == EVENT_BAND_UP ? 1 : -1);
    apply_range();
    jtty_rx_reset();
    lv_waterfall_clear_data(waterfall);
    txt_clear();
}

static void dial_apply(void)
{
    uint32_t f;

    if (cur_band < 0)
        return;
    f = (uint32_t)((int32_t)bands[cur_band].dial_hz + dial_offset_hz);
    cparam_i_set(cfg_fg_freq, (int32_t)f);
    lv_waterfall_clear_data(waterfall);
    jtty_rx_reset();
    msg_update_text_fmt("JTTY %s  %u.%03u MHz", bands[cur_band].label, (unsigned)(f / 1000000u),
                        (unsigned)((f / 1000u) % 1000u));
    refresh_btn(&btn_dial);
}

static void knob_release(void)
{
    if (knob_timer) {
        lv_timer_del(knob_timer);
        knob_timer = NULL;
    }
    if (knob_dial) {
        knob_dial = false;
        if (btn_dial.disp_btn)
            buttons_mark(&btn_dial, false);
        btn_dial.mark = false;
    }
}

static void knob_timer_cb(lv_timer_t *t)
{
    (void)t;
    knob_timer = NULL;
    if (alive) {
        knob_release();
        msg_update_text_fmt("Knob: fine tuning");
    }
}

static void knob_timer_restart(void)
{
    if (knob_timer) {
        lv_timer_reset(knob_timer);
    } else {
        knob_timer = lv_timer_create(knob_timer_cb, KNOB_HOLD_MS, NULL);
        lv_timer_set_repeat_count(knob_timer, 1);
    }
}

static const char *dial_label_getter(void)
{
    static char buf[32];

    if (dial_offset_hz == 0)
        snprintf(buf, sizeof(buf), "Dial\nstandard");
    else
        snprintf(buf, sizeof(buf), "Dial\n%+d kHz", (int)(dial_offset_hz / 1000));
    return buf;
}

/* Press: the multifunction knob moves the VFO in 1 kHz steps */
static void dial_press_cb(button_data_t *b)
{
    if (knob_dial) {
        knob_release();
        msg_update_text_fmt("Knob: fine tuning");
        return;
    }
    knob_dial = true;
    if (b->disp_btn)
        buttons_mark(b, true);
    b->mark = true;
    knob_timer_restart();
    msg_update_text_fmt("Knob: dial, 1 kHz steps");
}

/* Hold: back to the band's JTTY frequency */
static void dial_hold_cb(button_data_t *b)
{
    (void)b;
    if (jtty_tx_is_on()) {
        msg_update_text_fmt("#%06x JTTY: stop TX first#", theme_hex(TC_err));
        return;
    }
    if (dial_offset_hz != 0) {
        dial_offset_hz = 0;
        dial_apply();
    }
}

static void dial_change(int d)
{
    int32_t v = dial_offset_hz + d * DIAL_STEP_HZ;

    if (jtty_tx_is_on()) {
        msg_update_text_fmt("#%06x JTTY: stop TX first#", theme_hex(TC_err));
        return;
    }
    if (v < -DIAL_MAX_HZ)
        v = -DIAL_MAX_HZ;
    if (v > DIAL_MAX_HZ)
        v = DIAL_MAX_HZ;
    knob_timer_restart();
    if (v == dial_offset_hz)
        return;
    dial_offset_hz = v;
    dial_apply();
}

/* ---- Button labels and the small buttons ------------------------------- */

static const char *status_label_getter(void)
{
    static char buf[32];

    if (jtty_tx_is_on())
        snprintf(buf, sizeof(buf), "TX\n%u Hz", (unsigned)jtty_tx_get_freq());
    else
        snprintf(buf, sizeof(buf), "RX\n%u Hz", (unsigned)jtty_tx_get_freq());
    return buf;
}

static const char *dx_label_getter(void)
{
    static char buf[32];
    const char *c = digi_qso_dx();

    snprintf(buf, sizeof(buf), "DX call\n%s", (c && *c) ? c : "---");
    return buf;
}

static const char *serial_label_getter(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), "Serial\n%03d", serial);
    return buf;
}

static const char *tol_label_getter(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), "Rx tol\n%u Hz", (unsigned)tols[tol_idx]);
    return buf;
}

static void serial_press_cb(button_data_t *b)
{
    if (serial < 9999)
        serial++;
    buttons_refresh(b);
}

static void serial_hold_cb(button_data_t *b)
{
    serial = 1;
    buttons_refresh(b);
}

static void tol_press_cb(button_data_t *b)
{
    tol_idx = (tol_idx + 1) % (int)(sizeof(tols) / sizeof(tols[0]));
    jtty_rx_set_freq((float)jtty_tx_get_freq(), tols[tol_idx]);
    buttons_refresh(b);
}

static void status_timer_cb(lv_timer_t *t)
{
    bool tx = jtty_tx_is_on();

    (void)t;
    /* What the receiver heard while we were on the air is our own signal */
    if (was_tx && !tx)
        jtty_rx_reset();
    if (was_tx || tx)
        draft_dirty = true;
    was_tx = tx;
    refresh_btn(&btn_status);
}

/* ---- Keyboard focus around text entry (as dialog_psk.c) --------------- */

static void input_begin(void)
{
    input_active = true;
    if (text_box)
        lv_group_remove_obj(text_box);
}

static void focus_timer_cb(lv_timer_t *t)
{
    (void)t;
    focus_timer = NULL;
    if (!alive || !text_box || input_active)
        return;
    lv_group_add_obj(keyboard_group, text_box);
    lv_group_focus_obj(text_box);
    lv_group_set_editing(keyboard_group, true);
}

static void input_end(void)
{
    input_active = false;
    if (!focus_timer) {
        focus_timer = lv_timer_create(focus_timer_cb, 100, NULL);
        lv_timer_set_repeat_count(focus_timer, 1);
    }
}

static bool input_cancel_cb(void)
{
    input_end();
    return true;
}

static void textarea_set_copy(const char *text)
{
    static char buf[JTTY_MSG_LEN + 1];

    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    textarea_window_set(buf);
}

/* ---- Transmit --------------------------------------------------------- */

static void send_text(const char *text, int profile)
{
    char canon[JTTY_MSG_LEN + 1];

    if (!text || !*text)
        return;
    if (!jtty_tx_send(text, profile, canon)) {
        msg_update_text_fmt("#%06x JTTY: TX queue full#", theme_hex(TC_err));
        return;
    }
    txt_line_t *l = txt_add(2, 0);
    snprintf(l->s, sizeof(l->s), "%s", canon);
    was_tx = true;
    draft_dirty = true;
    refresh_btn(&btn_status);
}

static void send_draft(void)
{
    if (!draft[0])
        return;
    send_text(draft, JTTY_PROFILE_UNKNOWN);
    draft[0] = '\0';
    draft_dirty = true;
}

/* ---- F1-F8 macros ------------------------------------------------------ */

/* Copy with truncation; tabs and line breaks would break the file */
static void fkey_copy(char *dst, size_t sz, const char *src)
{
    size_t o = 0;

    for (; src && *src && o + 1 < sz; src++)
        dst[o++] = (*src == '\t' || *src == '\r' || *src == '\n') ? ' ' : *src;
    dst[o] = '\0';
}

static void fkey_set(int i, const char *name, const char *text)
{
    fkey_copy(fkey_name[i], sizeof(fkey_name[i]), name);
    fkey_copy(fkey_text[i], sizeof(fkey_text[i]), text);
}

static void fkey_labels(void)
{
    for (int i = 0; i < FKEYS; i++) {
        if (i < fkey_count)
            snprintf(fkey_label[i], sizeof(fkey_label[i]), "F%d\n%s", i + 1, fkey_name[i]);
        else
            snprintf(fkey_label[i], sizeof(fkey_label[i]), "F%d", i + 1);
        refresh_btn(fkey_btns[i]);
    }
}

static bool fkeys_save(void)
{
    FILE *f = fopen(JTTY_MACROS_PATH, "w");
    bool  ok;

    if (!f)
        return false;
    for (int i = 0; i < fkey_count; i++)
        fprintf(f, "%s\t%s\n", fkey_name[i], fkey_text[i]);
    ok = (fflush(f) == 0);
    if (ok)
        fsync(fileno(f));       /* /mnt is FAT on the SD card */
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

/* WSJT-X's JTTY function keys are the defaults; %Q (the next caller) is the DX call here */
static void fkeys_load(void)
{
    static const char *const def[FKEYS][2] = {
        {"CQ", "CQ %M CQ"}, {"Call 599", "%H %E"}, {"TU CQ", "%H TU CQ %M CQ"}, {"My call", "%M"},
        {"His call", "%H"}, {"TU NOW", "TU NOW %H %E"}, {"AGN?", "%H AGN?"}, {"599 Nr", "%E"},
    };
    char  line[FKEY_NAME + JTTY_MSG_LEN + 8];
    FILE *f = fopen(JTTY_MACROS_PATH, "r");

    fkey_count = 0;
    if (!f) {
        for (; fkey_count < FKEYS; fkey_count++)
            fkey_set(fkey_count, def[fkey_count][0], def[fkey_count][1]);
        fkeys_save();
    } else {
        while (fkey_count < FKEYS && fgets(line, sizeof(line), f)) {
            char *tab;

            line[strcspn(line, "\r\n")] = '\0';
            tab = strchr(line, '\t');
            if (!tab)
                continue;
            *tab = '\0';
            fkey_set(fkey_count++, line, tab + 1);
        }
        fclose(f);
    }
    fkey_labels();
}

/* Editor, as PSK's: name, then text. An empty text deletes the key's macro
 * and the ones after it move up. */
static bool fkey_text_ok_cb(void)
{
    const char *t = textarea_window_get();

    if (!alive || edit_key < 0) {
        input_end();
        return true;
    }
    if (!t || !*t) {
        if (edit_key < fkey_count) {
            for (int i = edit_key; i < fkey_count - 1; i++)
                fkey_set(i, fkey_name[i + 1], fkey_text[i + 1]);
            fkey_count--;
            msg_update_text_fmt("Macro deleted");
        }
    } else {
        fkey_set(edit_key, edit_name, t);
        if (edit_key == fkey_count)
            fkey_count++;
        msg_update_text_fmt("Macro saved");
    }
    if (!fkeys_save())
        msg_update_text_fmt("#%06x Could not write %s#", theme_hex(TC_err), JTTY_MACROS_PATH);
    fkey_labels();
    edit_key = -1;
    input_end();
    return true;
}

static void editor_timer_cb(lv_timer_t *t)
{
    (void)t;
    editor_timer = NULL;
    if (!alive) {
        input_active = false;
        return;
    }
    textarea_window_open_w_label(fkey_text_ok_cb, input_cancel_cb, "Text (%M %H %E):");
    lv_textarea_set_max_length(textarea_window_text(), JTTY_MSG_LEN);
    if (edit_key < fkey_count)
        textarea_set_copy(fkey_text[edit_key]);
}

static bool fkey_name_ok_cb(void)
{
    const char *t = textarea_window_get();

    if (!alive) {
        input_end();
        return true;
    }
    if (t && *t)
        snprintf(edit_name, sizeof(edit_name), "%s", t);
    else if (edit_key < fkey_count)
        snprintf(edit_name, sizeof(edit_name), "%s", fkey_name[edit_key]);
    else
        snprintf(edit_name, sizeof(edit_name), "M%d", edit_key + 1);

    /* Let this window close before the next one opens (as PSK) */
    if (!editor_timer) {
        editor_timer = lv_timer_create(editor_timer_cb, 50, NULL);
        lv_timer_set_repeat_count(editor_timer, 1);
    }
    return true;
}

static void fkey_hold(int n)
{
    if (input_active || n < 1 || n > FKEYS)
        return;
    edit_key = n - 1 < fkey_count ? n - 1 : fkey_count;     /* an empty key: the next free one */
    input_begin();
    textarea_window_open_w_label(fkey_name_ok_cb, input_cancel_cb, "Macro name:");
    lv_textarea_set_max_length(textarea_window_text(), FKEY_NAME - 1);
    if (edit_key < fkey_count)
        textarea_set_copy(fkey_name[edit_key]);
}

static void fkey(int n)
{
    char out[JTTY_MSG_LEN + 1] = "", exch[16];
    const char *dx = digi_qso_dx(), *my = params.callsign.x, *tpl;
    size_t o = 0;

    if (input_active || n < 1 || n > FKEYS)
        return;
    if (n > fkey_count) {
        msg_update_text_fmt("F%d is empty - hold to set it", n);
        return;
    }
    tpl = fkey_text[n - 1];
    if (strstr(tpl, "%H") && !(dx && *dx)) {
        msg_update_text_fmt("#%06x No DX call - set it on page 1#", theme_hex(TC_err));
        return;
    }
    if (strstr(tpl, "%M") && !(my && *my)) {
        msg_update_text_fmt("#%06x No callsign - set it in Settings#", theme_hex(TC_err));
        return;
    }
    snprintf(exch, sizeof(exch), "599 %03d", serial);
    for (const char *p = tpl; *p && o < JTTY_MSG_LEN; p++) {
        const char *v = NULL;
        if (*p == '%' && p[1]) {
            v = p[1] == 'M' ? my : p[1] == 'H' ? dx : p[1] == 'E' ? exch : NULL;
        }
        if (v) {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%s", v);
            p++;
        } else {
            out[o++] = *p;
            out[o] = '\0';
        }
    }
    /* The contest profile: "599 <serial>" is one frame, as WSJT-X's native keys */
    send_text(out, JTTY_PROFILE_RTTY);
}

static bool free_text_ok_cb(void)
{
    const char *t = textarea_window_get();

    if (alive && t && *t)
        send_text(t, JTTY_PROFILE_UNKNOWN);
    input_end();
    return true;
}

static void text_press_cb(button_data_t *b)
{
    (void)b;
    if (input_active)
        return;
    input_begin();
    textarea_window_open_w_label(free_text_ok_cb, input_cancel_cb, "JTTY msg:");
    lv_textarea_set_max_length(textarea_window_text(), JTTY_MSG_LEN);
}

static void stop_press_cb(button_data_t *b)
{
    (void)b;
    jtty_tx_stop();
}

static void status_press_cb(button_data_t *b)
{
    (void)b;
    jtty_rx_reset();
    msg_update_text_fmt("JTTY RX restart");
}

static void clear_press_cb(button_data_t *b)
{
    (void)b;
    txt_clear();
    lv_waterfall_clear_data(waterfall);
}

static void status_hold_cb(button_data_t *b)
{
    clear_press_cb(b);
}

static bool dx_ok_cb(void)
{
    const char *t = textarea_window_get();

    if (alive && t && *t) {
        digi_qso_set_dx(t);
        refresh_btn(&btn_dx);
    }
    input_end();
    return true;
}

static void dx_press_cb(button_data_t *b)
{
    const char *c = digi_qso_dx();

    (void)b;
    if (input_active)
        return;
    input_begin();
    textarea_window_open_w_label(dx_ok_cb, input_cancel_cb, "DX call:");
    if (c && *c)
        textarea_set_copy(c);
}

static void dx_hold_cb(button_data_t *b)
{
    digi_qso_clear();
    msg_update_text_fmt("QSO cleared");
    buttons_refresh(b);
}

/* ---- Dialog callbacks ------------------------------------------------- */

_Static_assert(JTTY_RX_RATE == AUDIO_CAPTURE_RATE / AUDIO_DECIM, "JTTY receiver rate");

/* Audio thread */
static void audio_cb(unsigned int n, float *samples)
{
    pthread_mutex_lock(&spec_mux);
    if (spec_ok)
        psk_spec_process(&spec, samples, n, on_spec_line, NULL);
    pthread_mutex_unlock(&spec_mux);
    if (!jtty_tx_is_on())
        jtty_rx_put_audio(n, samples);
}

static void rotary_cb(int32_t diff)
{
    int32_t mag = diff < 0 ? -diff : diff;
    int32_t step = ROTARY_HZ * (mag > 6 ? 6 : (mag > 3 ? 3 : 1));

    if (!jtty_tx_is_on())
        set_freq((int)jtty_tx_get_freq() + (int)(diff * step));
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (key >= KEYBOARD_F1 && key <= KEYBOARD_F8) {
        fkey((int)(key - KEYBOARD_F1) + 1);
        return;
    }
    switch (key) {
        case LV_KEY_ESC:
            if (jtty_tx_is_on())
                jtty_tx_stop();
            else
                dialog_destruct();
            break;
        case LV_KEY_ENTER:
        case '\r':
            send_draft();
            break;
        case LV_KEY_BACKSPACE: {
            size_t n = strlen(draft);
            if (n)
                draft[n - 1] = '\0';
            draft_dirty = true;
            break;
        }
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        /* Multifunction knob: the VFO while Dial has it, else 1 Hz tuning */
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT: {
            int d = (key == LV_KEY_RIGHT) ? 1 : -1;
            if (knob_dial)
                dial_change(d);
            else if (!jtty_tx_is_on())
                set_freq((int)jtty_tx_get_freq() + d);
            break;
        }
        default:
            if (key >= ' ' && key < 127) {
                size_t n = strlen(draft);
                char   c = (char)toupper((int)key);
                if (n < DRAFT_LEN && (c != ' ' || (n > 0 && draft[n - 1] != ' '))) {
                    draft[n] = c;
                    draft[n + 1] = '\0';
                    draft_dirty = true;
                }
            }
            break;
    }
}

/* MFK press (the encoder sends a click): send the line, or type one on screen */
static void click_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();

    (void)e;
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER)
        return;
    if (draft[0])
        send_draft();
    else
        text_press_cb(&btn_text);
}

static lv_obj_t *panel_box(lv_coord_t y, lv_coord_t h)
{
    lv_obj_t *box = lv_obj_create(dialog.obj);

    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 13, y);
    lv_obj_set_size(box, WIDTH, h);
    lv_obj_set_style_bg_color(box, TC(panel), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, TC(dialog_border), 0);
    lv_obj_set_style_border_opa(box, 96, 0);
    lv_obj_set_style_pad_all(box, TEXT_PAD, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static void construct_cb(lv_obj_t *parent)
{
    lv_coord_t tx_h;

    dialog.obj = dialog_init(parent);

    dsp_set_waterfall_enabled(false);
    dsp_set_spectrum_enabled(false);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    buttons_load_page(&page_1);

    /* Waterfall and the finder on the operator's frequency */
    waterfall = lv_waterfall_create(dialog.obj);
    lv_obj_add_style(waterfall, &waterfall_style, 0);
    lv_obj_clear_flag(waterfall, LV_OBJ_FLAG_SCROLLABLE);
    lv_waterfall_set_palette(waterfall, (lv_color_t *)wf_palette, 256);
    lv_waterfall_set_size(waterfall, WIDTH, WF_HEIGHT);
    lv_waterfall_set_min(waterfall, -3);
    lv_waterfall_set_max(waterfall, 25);
    lv_obj_set_pos(waterfall, 13, 13);

    finder = lv_finder_create(waterfall);
    lv_obj_set_size(finder, WIDTH, WF_HEIGHT);
    lv_obj_set_pos(finder, 0, 0);
    lv_obj_set_style_radius(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(finder, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(finder, TC(sel_bg), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(finder, LV_OPA_50, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(finder, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(finder, TC(text), LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(finder, LV_OPA_50, LV_PART_INDICATOR);
    lv_finder_clear_cursor(finder);

    /* Received text, and the TX line under it */
    txt_font = param_i_get(cfg_digi_text_compact) ? &sony_18 : &sony_24;
    tx_h = lv_font_get_line_height(txt_font) + 2 * TEXT_PAD + 2;
    text_h = DIALOG_H - TEXT_Y - 13 - GAP - tx_h;

    text_box = panel_box(TEXT_Y, text_h);
    for (int i = 0; i < TXT_SHOW; i++) {
        lv_obj_t *lbl = lv_label_create(text_box);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, WIDTH - 2 * TEXT_PAD - 2);
        lv_obj_set_style_text_font(lbl, txt_font, 0);
        lv_obj_set_style_text_color(lbl, TC(text), 0);
        lv_label_set_text(lbl, "");
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        line_lbl[i] = lbl;
    }

    tx_box = panel_box(TEXT_Y + text_h + GAP, tx_h);
    tx_lbl = lv_label_create(tx_box);
    lv_label_set_long_mode(tx_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(tx_lbl, WIDTH - 2 * TEXT_PAD - 2 - 90);
    lv_obj_set_style_text_font(tx_lbl, txt_font, 0);
    lv_obj_set_style_text_color(tx_lbl, TC(text), 0);
    lv_obj_align(tx_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    tx_state = lv_label_create(tx_box);
    lv_obj_set_style_text_font(tx_state, txt_font, 0);
    lv_label_set_text(tx_state, "");
    lv_obj_align(tx_state, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_add_event_cb(text_box, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(text_box, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(keyboard_group, text_box);
    lv_group_focus_obj(text_box);
    lv_group_set_editing(keyboard_group, true);

    fkeys_load();
    load_band(0);
    main_screen_lock_ab(true);
    main_screen_lock_mode(true);
    main_screen_lock_freq(true);
    main_screen_lock_band(true);

    txt_clear();
    draft[0] = '\0';
    draft_dirty = true;
    input_active = false;
    was_tx = false;
    knob_dial = false;

    pthread_mutex_lock(&spec_mux);
    spec_ok = (psk_spec_init(&spec, (float)JTTY_RX_RATE) == 0);
    pthread_mutex_unlock(&spec_mux);

    alive = true;
    jtty_rx_start(rx_msg_cb);
    apply_range();

    status_timer = lv_timer_create(status_timer_cb, TRACK_PERIOD_MS, NULL);
    text_timer = lv_timer_create(text_timer_cb, 100, NULL);
}

static void del_timer(lv_timer_t **t)
{
    if (*t) {
        lv_timer_del(*t);
        *t = NULL;
    }
}

static void destruct_cb(void)
{
    alive = false;

    del_timer(&status_timer);
    del_timer(&text_timer);
    del_timer(&focus_timer);
    del_timer(&editor_timer);
    del_timer(&knob_timer);
    knob_dial = false;
    btn_dial.mark = false;

    if (input_active) {
        textarea_window_close();
        input_active = false;
    }

    /* The transmission stops (tx_player unkeys, the VFO back) before the
     * frequency is put back */
    jtty_tx_abort();
    for (int i = 0; i < 100 && jtty_tx_is_on(); i++)
        usleep(20000);

    jtty_rx_stop();

    pthread_mutex_lock(&spec_mux);
    if (spec_ok)
        psk_spec_free(&spec);
    spec_ok = false;
    pthread_mutex_unlock(&spec_mux);

    waterfall = NULL;
    finder = NULL;
    text_box = NULL;
    tx_box = NULL;
    tx_lbl = NULL;
    tx_state = NULL;
    memset(line_lbl, 0, sizeof(line_lbl));

    dsp_set_waterfall_enabled(true);
    dsp_set_spectrum_enabled(true);

    main_screen_lock_ab(false);
    main_screen_lock_mode(false);
    main_screen_lock_freq(false);
    main_screen_lock_band(false);
}
