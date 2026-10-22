/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Olivia window
 *
 *  From SP2KMO's OpenX6100 psk31/olivia work (randal007 merge).
 *  Here: opened from the Modes menu, 8/250 by default, audio from the
 *  app's callback at 11025 Hz (resampled in olivia_app.c), TX through
 *  tx_player (power cap, ALC, SWR protection, TX strip), theme colours
 *  only, frequency and mode given back at the close by the framework.
 *
 *  Display and control layer only. The receiver (olivia_app.c over
 *  olivia_rx.c) and the transmitter (olivia_tx.c) live below; this file
 *  feeds them a variant and a frequency and shows what they produce.
 *
 *  Modelled on dialog_ft8.c: a full screen dialog owning a
 *  waterfall, with the received text in a panel under it.
 *
 *  Tuning: the radio stays on the band's Olivia dial frequency and the
 *  encoder moves the band (the finder on the waterfall, as wide as the
 *  variant) across the audio passband. Nothing snaps onto a signal:
 *  the receiver itself searches +-4 tones around the finder
 *  (+-125 Hz at 32/1000) and in both tone orientations. Every change
 *  of the centre or the variant rebuilds the receiver, which drops its
 *  synchronisation, so the rebuild waits until the encoder has stopped.
 *
 *  Olivia has no preamble: the receiver needs about four blocks (some
 *  8 s at 32/1000) to lock, and prints nothing until it has. That is
 *  also why there is no squelch here - without a lock there is no text.
 */

#include "dialog_olivia.h"

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

#include "olivia_app.h"
#include "olivia_tx.h"
#include "olivia_macro.h"
#include "psk_spec.h"           /* waterfall spectrum (also used by PSK31) */
#include "digi_qso.h"           /* shared QSO state: DX call and RST */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIDTH           771
/* Here: the waterfall on top, the text in a panel of its own under it
 * (upstream laid the text over the lower waterfall) */
#define WF_HEIGHT       118
#define TEXT_Y          (13 + WF_HEIGHT + 6)
#define TEXT_H          (DIALOG_H - TEXT_Y - 13)

#define TEXT_PAD        6

/* Encoder: Hz per click, times 3 or 6 when turned quickly. The receiver
 * searches +-4 tones itself, so the landing point need not be exact. */
#define ROTARY_HZ       10
#define FINE_HZ         5

/* The receiver is rebuilt this long after the last click. */
#define REBUILD_DELAY_MS 600

#define STATUS_PERIOD_MS 500

/* Received text: a ring of logical lines. LVGL wraps each one to the
 * width of the box; lines longer than this are broken by hand. */
#define TXT_LINES       24
#define TXT_LINE_LEN    96
#define TXT_SHOW        10      /* labels; more than the box can hold */


/*
 * Dial: the radio normally stays on the band's Olivia frequency and the
 * waterfall shows the passband above it. The Dial button lets the MFK
 * knob move the VFO in 1 kHz steps, up to 50 kHz either way, for a
 * station outside that window. Changing band, or holding Dial, returns
 * to the standard frequency; leaving the window restores whatever the
 * radio was on before it opened.
 */
#define DIAL_STEP_HZ    1000
#define DIAL_MAX_HZ     50000

#define KNOB_HOLD_MS    8000    /* MFK knob goes back to fine tuning */

/* ------------------------------------------------------------------ */
/* Olivia dial frequencies (USB)                                       */

/*
 * From the frequency table of oliviadigitalmode.org: dial frequencies
 * for the calling channels, with the waterfall centre at 1500 Hz, which
 * is where opening the window and every band change put the finder.
 * Region 1 values where regions differ (160 m 1837.5, not 1825.5; 40 m
 * the secondary 7071.0, since 7038.5 is a Region 2 channel). 20 m has a
 * second, 32/1000 channel. No 6 m channel is listed.
 */
typedef struct {
    const char *label;
    uint32_t    dial_hz;
} olivia_band_t;

static const olivia_band_t olivia_bands[] = {
    {"160m",       1837500},
    {"80m",        3581500},
    {"40m",        7071000},
    {"30m",       10141500},
    {"20m",       14071000},
    {"20m wide",  14107000},
    {"17m",       18097500},
    {"15m",       21071000},
    {"12m",       24921000},
    {"10m",       28121000},
};

#define OLIVIA_BAND_CNT ((int)(sizeof(olivia_bands) / sizeof(olivia_bands[0])))
#define BAND_CENTER_HZ  1500

static int cur_band = -1;

/* ------------------------------------------------------------------ */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);
static void audio_cb(unsigned int n, float *samples);
static void band_cb(lv_event_t *e);

static lv_obj_t   *waterfall = NULL;
static lv_obj_t   *finder = NULL;
static lv_obj_t   *text_box = NULL;
static lv_obj_t   *line_lbl[TXT_SHOW];

/* Received text font: as the RTTY/CW panel, Settings > Display > Decoder text */
static const lv_font_t *txt_font = &sony_24;

static lv_timer_t *status_timer = NULL;
static lv_timer_t *text_timer = NULL;
static lv_timer_t *rebuild_timer = NULL;
static lv_timer_t *focus_timer = NULL;
static lv_timer_t *editor_timer = NULL;
static lv_timer_t *knob_timer = NULL;


/* True between construct and destruct. Work the DSP thread queued for
 * the UI thread can still arrive after the window is gone. */
static bool        alive = false;

/* Spectrum: the DSP thread runs psk_spec through the receiver's audio
 * tap; the UI thread changes its range. */
static pthread_mutex_t spec_mux = PTHREAD_MUTEX_INITIALIZER;
static psk_spec_t  spec;
static bool        spec_ok = false;

/* UI thread copies of the displayed span. */
static float       sp_edge_lo = 200.0f;
static float       sp_edge_hi = 2800.0f;

/* What the multifunction knob does: fine tuning normally, the VFO while
 * the Dial button has it. */
typedef enum { KNOB_FINE, KNOB_DIAL } knob_mode_t;
static knob_mode_t knob = KNOB_FINE;
static int32_t     dial_offset_hz = 0; /* from the band's Olivia dial */

/* Text entry state */
static bool        input_active = false;
static int         edit_idx = -1;      /* macro being edited, -1 = new */
static char        edit_name[OLIVIA_MACRO_NAME_LEN];

/* ---- Buttons ---------------------------------------------------------- */

static const char *status_label_getter(void);
static const char *mode_label_getter(void);
static const char *dx_label_getter(void);
static const char *rst_label_getter(void);
static const char *dial_label_getter(void);

static void text_press_cb(button_data_t *b);
static void stop_press_cb(button_data_t *b);
static void mode_press_cb(button_data_t *b);
static void status_press_cb(button_data_t *b);
static void status_hold_cb(button_data_t *b);
static void band_up_cb(button_data_t *b);
static void band_down_cb(button_data_t *b);
static void dial_press_cb(button_data_t *b);
static void dial_hold_cb(button_data_t *b);
static void dx_press_cb(button_data_t *b);
static void dx_hold_cb(button_data_t *b);
static void rst_press_cb(button_data_t *b);
static void rst_hold_cb(button_data_t *b);

static void macro_press(int idx);
static void macro_hold(int idx);

static char macro_labels[OLIVIA_MACRO_MAX][OLIVIA_MACRO_NAME_LEN + 2];

static buttons_page_t page_1, page_2, page_3, page_4, page_5;

static button_data_t btn_p1 = {
    .type = BTN_TEXT, .label = "(OLV 1:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb,
    .next = &page_2, .prev = &page_5,
};
static button_data_t btn_text = {
    .type = BTN_TEXT, .label = "Free\nText", .press = text_press_cb,
};
static button_data_t btn_stop = {
    .type = BTN_TEXT, .label = "Stop\nTX", .press = stop_press_cb,
};
static button_data_t btn_mode = {
    .type = BTN_TEXT_FN, .label_fn = mode_label_getter,
    .press = mode_press_cb,
};
static button_data_t btn_status = {
    .type = BTN_TEXT_FN, .label_fn = status_label_getter,
    .press = status_press_cb, .hold = status_hold_cb,
};

static button_data_t btn_p2 = {
    .type = BTN_TEXT, .label = "(OLV 2:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb,
    .next = &page_3, .prev = &page_1,
};
/* One button for both directions: press up, hold down. The radio's
 * BAND keys do the same thing. */
static button_data_t btn_band = {
    .type = BTN_TEXT, .label = "Band up\nhold: down",
    .press = band_up_cb, .hold = band_down_cb,
};
static button_data_t btn_dial = {
    .type = BTN_TEXT_FN, .label_fn = dial_label_getter,
    .press = dial_press_cb, .hold = dial_hold_cb,
};
static button_data_t btn_dx = {
    .type = BTN_TEXT_FN, .label_fn = dx_label_getter,
    .press = dx_press_cb, .hold = dx_hold_cb,
};
static button_data_t btn_rst = {
    .type = BTN_TEXT_FN, .label_fn = rst_label_getter,
    .press = rst_press_cb, .hold = rst_hold_cb,
};

/* Macro buttons: press sends, hold edits (or creates, on an empty slot).
 * button_data_t has no user pointer, so one pair of callbacks per slot. */
#define OLV_MACRO_BTN(n)                                                      \
    static void macro##n##_press_cb(button_data_t *b) { (void)b; macro_press(n); } \
    static void macro##n##_hold_cb(button_data_t *b)  { (void)b; macro_hold(n); }  \
    static button_data_t btn_macro##n = {                                     \
        .type  = BTN_TEXT,                                                    \
        .label = macro_labels[n],                                             \
        .press = macro##n##_press_cb,                                         \
        .hold  = macro##n##_hold_cb,                                          \
    };

OLV_MACRO_BTN(0)
OLV_MACRO_BTN(1)
OLV_MACRO_BTN(2)
OLV_MACRO_BTN(3)
OLV_MACRO_BTN(4)
OLV_MACRO_BTN(5)
OLV_MACRO_BTN(6)
OLV_MACRO_BTN(7)
OLV_MACRO_BTN(8)
OLV_MACRO_BTN(9)
OLV_MACRO_BTN(10)
OLV_MACRO_BTN(11)

#undef OLV_MACRO_BTN

static button_data_t *const macro_btns[OLIVIA_MACRO_MAX] = {
    &btn_macro0, &btn_macro1, &btn_macro2,  &btn_macro3,
    &btn_macro4, &btn_macro5, &btn_macro6,  &btn_macro7,
    &btn_macro8, &btn_macro9, &btn_macro10, &btn_macro11,
};

static button_data_t btn_p3 = {
    .type = BTN_TEXT, .label = "(OLV 3:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb,
    .next = &page_4, .prev = &page_2,
};
static button_data_t btn_p4 = {
    .type = BTN_TEXT, .label = "(OLV 4:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb,
    .next = &page_5, .prev = &page_3,
};
static button_data_t btn_p5 = {
    .type = BTN_TEXT, .label = "(OLV 5:5)",
    .press = button_next_page_cb, .hold = button_prev_page_cb,
    .next = &page_1, .prev = &page_4,
};

static buttons_page_t page_1 = {
    {&btn_p1, &btn_text, &btn_stop, &btn_mode, &btn_status}
};
static buttons_page_t page_2 = {
    {&btn_p2, &btn_band, &btn_dial, &btn_dx, &btn_rst}
};
static buttons_page_t page_3 = {
    {&btn_p3, &btn_macro0, &btn_macro1, &btn_macro2, &btn_macro3}
};
static buttons_page_t page_4 = {
    {&btn_p4, &btn_macro4, &btn_macro5, &btn_macro6, &btn_macro7}
};
static buttons_page_t page_5 = {
    {&btn_p5, &btn_macro8, &btn_macro9, &btn_macro10, &btn_macro11}
};

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .audio_cb = audio_cb,
    .rotary_cb = rotary_cb,
    .key_cb = key_cb,
    .restore_radio = true,  /* frequency and mode back at the close */
};

dialog_t *dialog_olivia = &dialog;

/* BTN_TEXT_FN without a subject never repaints by itself, and a button
 * that is not on the current page has nothing to repaint. */
static void refresh_btn(button_data_t *b)
{
    if (b && b->disp_btn)
        buttons_refresh(b);
}

/* ---- Received text ---------------------------------------------------- */

typedef struct {
    char    s[TXT_LINE_LEN];
    uint8_t len;
    bool    tx;
} txt_line_t;

static txt_line_t txt[TXT_LINES];
static int        txt_head = 0;     /* line being written */
static int        txt_cnt = 1;      /* lines in use, including head */
static bool       txt_dirty = false;

static void txt_clear(void)
{
    memset(txt, 0, sizeof(txt));
    txt_head = 0;
    txt_cnt = 1;
    txt_dirty = true;
}

static void txt_new_line(bool tx)
{
    txt_head = (txt_head + 1) % TXT_LINES;
    txt[txt_head].len = 0;
    txt[txt_head].s[0] = '\0';
    txt[txt_head].tx = tx;
    if (txt_cnt < TXT_LINES)
        txt_cnt++;
    txt_dirty = true;
}

static void txt_put(char c, bool tx)
{
    txt_line_t *l = &txt[txt_head];

    /* Received text never continues a transmitted line, and the other
     * way round, so the colours never mix within one line. */
    if (l->tx != tx) {
        if (l->len > 0) {
            txt_new_line(tx);
            l = &txt[txt_head];
        } else {
            l->tx = tx;
        }
    }

    if (c == '\n') {
        txt_new_line(tx);
        return;
    }

    if (l->len + 1 >= TXT_LINE_LEN) {
        txt_new_line(tx);
        l = &txt[txt_head];
    }

    l->s[l->len++] = c;
    l->s[l->len] = '\0';
    txt_dirty = true;
}

/*
 * One label per logical line rather than one label with colour markup:
 * LVGL 8 resets the recolour state at every wrapped row, so a sent line
 * long enough to wrap would lose its colour half way. The labels are
 * stacked upwards from the bottom of the box, which clips whatever no
 * longer fits at the top.
 */
static void txt_render(void)
{
    int first = (txt_head - txt_cnt + 1 + TXT_LINES) % TXT_LINES;
    int n = txt_cnt;
    int k;

    /* An empty line being written is the next line, not a blank one:
     * leave it out, or the text would sit one row too high. */
    if (n > 0 && txt[txt_head].len == 0)
        n--;

    /* Only the newest few can be on screen. */
    if (n > TXT_SHOW) {
        first = (first + (n - TXT_SHOW)) % TXT_LINES;
        n = TXT_SHOW;
    }

    for (k = n; k < TXT_SHOW; k++)
        lv_obj_add_flag(line_lbl[k], LV_OBJ_FLAG_HIDDEN);

    /* Newest at the bottom, working upwards. */
    lv_coord_t y = TEXT_H - 2 * TEXT_PAD - 2;   /* 1 px border */

    for (k = n - 1; k >= 0; k--) {
        const txt_line_t *l = &txt[(first + k) % TXT_LINES];
        lv_obj_t *lbl = line_lbl[k];
        lv_point_t size;

        lv_txt_get_size(&size, l->s, txt_font, 0, 0, WIDTH - 2 * TEXT_PAD - 2,
                        LV_TEXT_FLAG_NONE);
        y -= size.y;

        /* A line that no longer fits whole is dropped rather than shown
         * cut in half, and so is everything older. */
        if (y < 0) {
            for (int j = 0; j <= k; j++)
                lv_obj_add_flag(line_lbl[j], LV_OBJ_FLAG_HIDDEN);
            break;
        }

        lv_label_set_text(lbl, l->s);
        lv_obj_set_style_text_color(lbl, l->tx ? TC(tx)
                                               : TC(text), 0);
        lv_obj_set_pos(lbl, 0, y);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void text_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (txt_dirty && text_box) {
        txt_dirty = false;
        txt_render();
    }
}

/* UI thread: one decoded block, at most 8 characters. */
static void rx_text_ui(void *data)
{
    const char *s = (const char *)data;
    char before[16];

    if (!alive)
        return;

    snprintf(before, sizeof(before), "%s", digi_qso_dx());

    for (; *s; s++) {
        txt_put(*s, false);
        /* Catch the correspondent's call from "DE <call>". */
        const char *de = digi_qso_feed_rx(*s);
        if (de && param_i_get(cfg_pskreporter))     /* no S/N figure in these modes: 0 dB */
            pskr_spot(params.callsign.x, station_qth(), VERSION, de, "",
                      (uint32_t)cparam_i_get(cfg_fg_freq) + olivia_tx_get_center(), 0, "OLIVIA");
    }

    if (strcmp(before, digi_qso_dx()) != 0)
        refresh_btn(&btn_dx);
}

/* DSP thread, receiver lock held: hand over and return. */
static void rx_text_cb(const char *text)
{
    scheduler_put(rx_text_ui, (void *)text, strlen(text) + 1);
}

/* UI thread, from olivia_tx_send(). */
static void tx_echo_cb(const char *text)
{
    const char *p = text;
    size_t      n;

    if (!alive || !text)
        return;

    /* The line breaks sent around every transmission are for the other
     * station's screen; here each transmission gets its own line. */
    while (*p == '\n' || *p == '\r')
        p++;
    n = strlen(p);
    while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r'))
        n--;

    if (txt[txt_head].len > 0)
        txt_new_line(true);

    for (size_t i = 0; i < n; i++) {
        if (p[i] != '\r')
            txt_put(p[i], true);
    }
    txt_new_line(false);
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

/* DSP thread, from inside the tap. */
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
    scheduler_put(wf_line_ui, &l, sizeof(l));
}

/* DSP thread, receiver lock held. */
static void rx_tap(unsigned int n, const float *samples)
{
    pthread_mutex_lock(&spec_mux);
    if (spec_ok)
        psk_spec_process(&spec, samples, n, on_spec_line, NULL);
    pthread_mutex_unlock(&spec_mux);
}

static int cur_bandwidth(void)
{
    return olivia_tx_mode_bandwidth(olivia_tx_get_mode());
}

static int center_min(void)
{
    return (int)ceilf(sp_edge_lo) + cur_bandwidth() / 2;
}

static int center_max(void)
{
    return (int)floorf(sp_edge_hi) - cur_bandwidth() / 2;
}

static void update_finder(void)
{
    int c = (int)olivia_tx_get_center();
    int bw = cur_bandwidth();

    lv_finder_set_range(finder, (int16_t)lroundf(sp_edge_lo),
                        (int16_t)lroundf(sp_edge_hi));
    lv_finder_set_width(finder, (uint16_t)bw);
    lv_finder_set_value(finder, (int16_t)(c - bw / 2));
    lv_obj_invalidate(finder);
}

/* Move the band without rebuilding the receiver yet. */
static void set_center(int hz)
{
    int lo = center_min();
    int hi = center_max();

    if (lo > hi)            /* a wide variant in a narrow filter */
        hz = (lo + hi) / 2;
    else if (hz < lo)
        hz = lo;
    else if (hz > hi)
        hz = hi;

    olivia_tx_set_center((uint16_t)hz);     /* clamps to its own 500..2500 */
    update_finder();
    refresh_btn(&btn_status);
}

static void rebuild_rx(void)
{
    olivia_app_update_settings();
}

static void rebuild_timer_cb(lv_timer_t *t)
{
    (void)t;
    rebuild_timer = NULL;      /* one-shot: LVGL deletes it */
    if (alive)
        rebuild_rx();
}

static void rebuild_later(void)
{
    if (rebuild_timer) {
        lv_timer_reset(rebuild_timer);
    } else {
        rebuild_timer = lv_timer_create(rebuild_timer_cb, REBUILD_DELAY_MS, NULL);
        lv_timer_set_repeat_count(rebuild_timer, 1);
    }
}

/* Waterfall span follows the receive filter of the current mode, as
 * it does in the FT8 window. */
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

    /* Keep the band inside the span. */
    set_center((int)olivia_tx_get_center());
}

/* ---- Knob ------------------------------------------------------------- */

static void knob_release(bool say)
{
    if (knob_timer) {
        lv_timer_del(knob_timer);
        knob_timer = NULL;
    }
    if (knob != KNOB_FINE) {
        if (btn_dial.disp_btn)
            buttons_mark(&btn_dial, false);
        btn_dial.mark = false;
        knob = KNOB_FINE;
        if (say)
            msg_update_text_fmt("Knob: fine tuning");
    }
}

static void knob_timer_cb(lv_timer_t *t)
{
    (void)t;
    knob_timer = NULL;      /* one-shot: LVGL deletes it */
    if (alive)
        knob_release(true);
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

/* ---- Band selection --------------------------------------------------- */

static int band_nearest(uint32_t freq)
{
    int best = 0;
    int64_t best_d = -1;

    for (int i = 0; i < OLIVIA_BAND_CNT; i++) {
        int64_t d = (int64_t)freq - (int64_t)olivia_bands[i].dial_hz;

        if (d < 0)
            d = -d;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static void band_msg(uint32_t f)
{
    if (dial_offset_hz == 0)
        msg_update_text_fmt("Olivia %s  %u.%04u MHz", olivia_bands[cur_band].label,
                            (unsigned)(f / 1000000u), (unsigned)((f / 100u) % 10000u));
    else
        msg_update_text_fmt("Olivia %s  %u.%04u MHz (%+d kHz)", olivia_bands[cur_band].label,
                            (unsigned)(f / 1000000u), (unsigned)((f / 100u) % 10000u),
                            (int)(dial_offset_hz / 1000));
}

/* dir 0 picks the band closest to where the radio already is, which is
 * what opening the window does; +-1 steps through the list. */
static void load_band(int8_t dir)
{
    int idx;

    if (dir == 0 || cur_band < 0) {
        idx = band_nearest((uint32_t)cparam_i_get(cfg_fg_freq));
    } else {
        idx = cur_band + dir;
        if (idx < 0)
            idx = OLIVIA_BAND_CNT - 1;
        if (idx >= OLIVIA_BAND_CNT)
            idx = 0;
    }

    cur_band = idx;
    dial_offset_hz = 0;
    refresh_btn(&btn_dial);

    /* Frequency first, then mode - the order cfg_digital_load() uses. */
    cparam_i_set(cfg_fg_freq, (int32_t)olivia_bands[idx].dial_hz);
    cparam_i_set(cfg_cur_mode, radio_mode_usb_dig);

    /* The listed dial frequencies assume the waterfall centre at 1500. */
    olivia_tx_set_center(BAND_CENTER_HZ);

    band_msg(olivia_bands[idx].dial_hz);
}

static void change_band(int8_t dir)
{
    if (input_active)
        return;

    if (olivia_tx_is_on()) {
        msg_update_text_fmt("#%06x Olivia: stop TX first#", theme_hex(TC_err));
        return;
    }

    load_band(dir);
    apply_range();
    rebuild_rx();
    lv_waterfall_clear_data(waterfall);
    txt_clear();
}

static void band_cb(lv_event_t *e)
{
    change_band(lv_event_get_code(e) == EVENT_BAND_UP ? 1 : -1);
}

static void band_up_cb(button_data_t *b)
{
    (void)b;
    change_band(1);
}

static void band_down_cb(button_data_t *b)
{
    (void)b;
    change_band(-1);
}

/* ---- Dial ------------------------------------------------------------- */

static const char *dial_label_getter(void)
{
    static char buf[32];

    if (dial_offset_hz == 0)
        snprintf(buf, sizeof(buf), "Dial\nstandard");
    else
        snprintf(buf, sizeof(buf), "Dial\n%+d kHz", (int)(dial_offset_hz / 1000));
    return buf;
}

static void dial_apply(void)
{
    uint32_t f;

    if (cur_band < 0)
        return;

    f = (uint32_t)((int32_t)olivia_bands[cur_band].dial_hz + dial_offset_hz);
    cparam_i_set(cfg_fg_freq, (int32_t)f);

    /* What the waterfall and the receiver hold belongs to the old dial.
     * The received text stays: it is still the same conversation. */
    lv_waterfall_clear_data(waterfall);
    rebuild_rx();

    band_msg(f);
    refresh_btn(&btn_dial);
}

static void dial_change(int d)
{
    int32_t v = dial_offset_hz + d * DIAL_STEP_HZ;

    if (olivia_tx_is_on()) {
        msg_update_text_fmt("#%06x Olivia: stop TX first#", theme_hex(TC_err));
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

/* Press: the multifunction knob moves the VFO in 1 kHz steps; press
 * again, or leave the knob for 8 s, to get fine tuning back. */
static void dial_press_cb(button_data_t *b)
{
    (void)b;

    if (knob == KNOB_DIAL) {
        knob_release(true);
        return;
    }
    knob = KNOB_DIAL;
    if (btn_dial.disp_btn)
        buttons_mark(&btn_dial, true);
    btn_dial.mark = true;
    knob_timer_restart();
    msg_update_text_fmt("Knob: dial, 1 kHz steps");
}

/* Hold: back to the band's Olivia frequency. */
static void dial_hold_cb(button_data_t *b)
{
    (void)b;

    if (olivia_tx_is_on()) {
        msg_update_text_fmt("#%06x Olivia: stop TX first#", theme_hex(TC_err));
        return;
    }
    if (dial_offset_hz != 0) {
        dial_offset_hz = 0;
        dial_apply();
    }
}

/* ---- Button labels ---------------------------------------------------- */

static const char *status_label_getter(void)
{
    static char buf[40];

    if (olivia_tx_is_on()) {
        snprintf(buf, sizeof(buf), "TX\n%u Hz", (unsigned)olivia_tx_get_center());
    } else if (!olivia_app_is_active()) {
        snprintf(buf, sizeof(buf), "RX off");
    } else if (olivia_app_locked()) {
        snprintf(buf, sizeof(buf), "LOCK %u%%\n%+.0f Hz",
                 (unsigned)olivia_app_quality(),
                 (double)olivia_app_freq_error());
    } else {
        /* The best hypothesis says whether there is any Olivia on the
         * air at all: about 3 is noise, above 4 something is there. */
        snprintf(buf, sizeof(buf), "Search\nbest %.1f",
                 (double)olivia_app_best_score());
    }
    return buf;
}

static const char *mode_label_getter(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), "Mode\n%s", olivia_tx_mode_name(olivia_tx_get_mode()));
    return buf;
}

static const char *dx_label_getter(void)
{
    static char buf[32];
    const char *c = digi_qso_dx();

    snprintf(buf, sizeof(buf), "DX call\n%s", (c && *c) ? c : "---");
    return buf;
}

static const char *rst_label_getter(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), "RST snt\n%s", digi_qso_rst_sent());
    return buf;
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_btn(&btn_status);
}

static void refresh_macro_labels(void)
{
    int n = olivia_macros_count();

    for (int i = 0; i < OLIVIA_MACRO_MAX; i++) {
        const olivia_macro_t *m = (i < n) ? olivia_macro_get(i) : NULL;

        snprintf(macro_labels[i], sizeof(macro_labels[i]), "%s",
                 (m && m->name[0]) ? m->name : "--");
        refresh_btn(macro_btns[i]);
    }
}

/* ---- Keyboard focus around text entry --------------------------------- */

/*
 * While a text window is open it must be the only thing in the
 * keyboard group, and afterwards the text box has to get the focus
 * back. main_screen_keys_enable() is not usable here - it hands the
 * keyboard to the main screen's spectrum, which is behind this window.
 */
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

/* Called from an OK or Cancel callback, which returns true so the text
 * window closes itself; the focus comes back once it has gone. */
static void input_end(void)
{
    input_active = false;
    if (!focus_timer) {
        focus_timer = lv_timer_create(focus_timer_cb, 100, NULL);
        lv_timer_set_repeat_count(focus_timer, 1);
    }
}

/* textarea_window_set() takes a plain char pointer; hand it a copy so
 * the stored macro can never be written through it. */
static void textarea_set_copy(const char *text)
{
    static char buf[OLIVIA_MACRO_TEXT_LEN];

    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    textarea_window_set(buf);
}

static bool input_cancel_cb(void)
{
    input_end();
    return true;
}

/* ---- Transmit --------------------------------------------------------- */

static void send_text(const char *text)
{
    char buf[OLIVIA_MACRO_TEXT_LEN + 400];

    if (!text || !*text)
        return;

    if (olivia_tx_is_on()) {
        msg_update_text_fmt("#%06x Olivia TX busy#", theme_hex(TC_err));
        return;
    }

    /* Start and finish on a fresh line on the other station's screen. */
    snprintf(buf, sizeof(buf), "\n%s\n", text);

    if (olivia_tx_send(buf)) {
        msg_update_text_fmt("Olivia %s TX...",
                            olivia_tx_mode_name(olivia_tx_get_mode()));
        refresh_btn(&btn_status);
    } else {
        msg_update_text_fmt("#%06x Olivia TX busy#", theme_hex(TC_err));
    }
}

static bool free_text_ok_cb(void)
{
    const char *t = textarea_window_get();

    if (alive && t && *t)
        send_text(t);
    input_end();
    return true;
}

static void text_press_cb(button_data_t *b)
{
    (void)b;

    if (input_active)
        return;
    if (olivia_tx_is_on()) {
        msg_update_text_fmt("#%06x Olivia TX busy#", theme_hex(TC_err));
        return;
    }

    input_begin();
    textarea_window_open_w_label(free_text_ok_cb, input_cancel_cb, "Olivia msg:");
    /* The stock window takes 64 characters. */
    lv_textarea_set_max_length(textarea_window_text(), OLIVIA_MACRO_TEXT_LEN - 1);
}

/* Olivia sends whole blocks: the current one is finished and one more
 * closes the transmission, about 4 s at 32/1000. */
static void stop_press_cb(button_data_t *b)
{
    (void)b;
    if (olivia_tx_is_on()) {
        olivia_tx_stop();
        msg_update_text_fmt("Olivia: stopping after this block");
    }
}

static void mode_press_cb(button_data_t *b)
{
    olivia_mode_t m;

    if (olivia_tx_is_on()) {
        msg_update_text_fmt("#%06x Olivia: stop TX first#", theme_hex(TC_err));
        return;
    }

    m = (olivia_mode_t)((olivia_tx_get_mode() + 1) % OLIVIA_MODE_COUNT);
    olivia_tx_set_mode(m);

    /* A wider variant may no longer fit around the old centre. */
    set_center((int)olivia_tx_get_center());
    rebuild_rx();

    msg_update_text_fmt("Olivia %s", olivia_tx_mode_name(m));
    buttons_refresh(b);
}

static void status_press_cb(button_data_t *b)
{
    (void)b;
    rebuild_rx();
    msg_update_text_fmt("Olivia RX resync");
}

/* Hold: clear the text and the waterfall. */
static void status_hold_cb(button_data_t *b)
{
    (void)b;
    txt_clear();
    lv_waterfall_clear_data(waterfall);
}

/* ---- Macros ----------------------------------------------------------- */

static void macro_press(int idx)
{
    const olivia_macro_t *m;
    char out[OLIVIA_MACRO_TEXT_LEN + 128];
    const char *missing = NULL;

    if (input_active)
        return;

    m = olivia_macro_get(idx);
    if (!m) {
        msg_update_text_fmt("Empty slot - hold to create a macro");
        return;
    }

    if (!olivia_macro_expand(m->text, out, sizeof(out), &missing)) {
        if (missing && strcmp(missing, "<CALL>") == 0)
            msg_update_text_fmt("#%06x No DX call - set it on page 2#", theme_hex(TC_err));
        else
            msg_update_text_fmt("#%06x %s is empty#", theme_hex(TC_err), missing ? missing : "?");
        return;
    }

    send_text(out);
}

/* Editor, two steps: name, then text. Both windows open pre-filled when
 * an existing macro is edited, so a macro can be renamed. Saving an
 * empty text deletes the macro. */

static bool editor_text_ok_cb(void)
{
    const char *t = textarea_window_get();
    bool empty = (!t || !*t);

    if (!alive) {
        input_end();
        return true;
    }

    if (edit_idx >= 0) {
        if (empty) {
            olivia_macro_delete(edit_idx);
            msg_update_text_fmt("Macro deleted");
        } else {
            olivia_macro_set(edit_idx, edit_name, t);
            msg_update_text_fmt("Macro saved");
        }
    } else if (!empty) {
        if (olivia_macro_add(edit_name, t) < 0)
            msg_update_text_fmt("#%06x All %d macro slots are used#", theme_hex(TC_err), OLIVIA_MACRO_MAX);
        else
            msg_update_text_fmt("Macro saved");
    }

    if (!olivia_macros_save())
        msg_update_text_fmt("#%06x Could not write /mnt/olivia_macros.txt#", theme_hex(TC_err));

    refresh_macro_labels();
    edit_idx = -1;
    input_end();
    return true;
}

static void editor_timer_cb(lv_timer_t *t)
{
    const olivia_macro_t *m;

    (void)t;
    editor_timer = NULL;

    if (!alive) {
        input_active = false;
        return;
    }

    textarea_window_open_w_label(editor_text_ok_cb, input_cancel_cb, "Macro text:");
    lv_textarea_set_max_length(textarea_window_text(), OLIVIA_MACRO_TEXT_LEN - 1);

    m = (edit_idx >= 0) ? olivia_macro_get(edit_idx) : NULL;
    if (m)
        textarea_set_copy(m->text);
}

static bool editor_name_ok_cb(void)
{
    const char *t = textarea_window_get();

    if (!alive) {
        input_end();
        return true;
    }

    if (t && *t) {
        snprintf(edit_name, sizeof(edit_name), "%s", t);
    } else {
        const olivia_macro_t *m = (edit_idx >= 0) ? olivia_macro_get(edit_idx) : NULL;

        if (m)
            snprintf(edit_name, sizeof(edit_name), "%s", m->name);
        else
            snprintf(edit_name, sizeof(edit_name), "M%d", olivia_macros_count() + 1);
    }

    /* Let this window close before the next one opens, or two text
     * areas end up in the keyboard group. input_active stays set. */
    if (!editor_timer) {
        editor_timer = lv_timer_create(editor_timer_cb, 50, NULL);
        lv_timer_set_repeat_count(editor_timer, 1);
    }
    return true;
}

static void macro_hold(int idx)
{
    const olivia_macro_t *m;

    if (input_active)
        return;

    if (idx >= olivia_macros_count()) {
        if (olivia_macros_count() >= OLIVIA_MACRO_MAX) {
            msg_update_text_fmt("#%06x All %d macro slots are used#", theme_hex(TC_err), OLIVIA_MACRO_MAX);
            return;
        }
        idx = -1;       /* an empty slot: new macro, added at the end */
    }

    edit_idx = idx;
    m = (idx >= 0) ? olivia_macro_get(idx) : NULL;

    input_begin();
    textarea_window_open_w_label(editor_name_ok_cb, input_cancel_cb, "Macro name:");
    if (m)
        textarea_set_copy(m->name);
}

/* ---- QSO fields ------------------------------------------------------- */

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
    refresh_btn(&btn_rst);
}

/* Only the strength digit changes; readability and tone stay 5 and 9. */
static void rst_press_cb(button_data_t *b)
{
    digi_qso_change_rst_sent(1);
    buttons_refresh(b);
}

static void rst_hold_cb(button_data_t *b)
{
    for (int i = 0; i < 9 && digi_qso_change_rst_sent(0) != 9; i++)
        digi_qso_change_rst_sent(1);
    buttons_refresh(b);
}

/* ---- Dialog callbacks ------------------------------------------------- */

_Static_assert(OLIVIA_IN_RATE == AUDIO_CAPTURE_RATE / AUDIO_DECIM, "Olivia input rate");

/* Audio thread */
static void audio_cb(unsigned int n, float *samples)
{
    olivia_app_put_audio_samples(n, samples);
}

/* Main encoder: moves the band; the receiver follows once it stops. */
static void rotary_cb(int32_t diff)
{
    int32_t mag = diff < 0 ? -diff : diff;
    int32_t step = ROTARY_HZ * (mag > 6 ? 6 : (mag > 3 ? 3 : 1));

    if (olivia_tx_is_on()) {
        msg_update_text_fmt("#%06x Olivia: stop TX first#", theme_hex(TC_err));
        return;
    }

    set_center((int)olivia_tx_get_center() + (int)(diff * step));
    rebuild_later();
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;

        /* Multifunction knob: the VFO while the Dial button has it,
         * otherwise fine tuning of the band. */
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT: {
            int d = (key == LV_KEY_RIGHT) ? 1 : -1;

            if (knob == KNOB_DIAL) {
                dial_change(d);
            } else if (!olivia_tx_is_on()) {
                set_center((int)olivia_tx_get_center() + d * FINE_HZ);
                rebuild_later();
            }
            break;
        }
    }
}

static void construct_cb(lv_obj_t *parent)
{
    dialog.obj = dialog_init(parent);

    /* The window covers the main screen entirely, so its spectrum and
     * waterfall would be computed for nothing. */
    dsp_set_waterfall_enabled(false);
    dsp_set_spectrum_enabled(false);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    buttons_load_page(&page_1);

    /* Waterfall */

    waterfall = lv_waterfall_create(dialog.obj);

    lv_obj_add_style(waterfall, &waterfall_style, 0);
    lv_obj_clear_flag(waterfall, LV_OBJ_FLAG_SCROLLABLE);

    lv_waterfall_set_palette(waterfall, (lv_color_t *)wf_palette, 256);
    lv_waterfall_set_size(waterfall, WIDTH, WF_HEIGHT);

    /* dB above the noise floor of each line, so fixed limits hold
     * whatever the AF and RF gain are. */
    lv_waterfall_set_min(waterfall, -3);
    lv_waterfall_set_max(waterfall, 25);

    lv_obj_set_pos(waterfall, 13, 13);

    /* The band the receiver listens to */

    finder = lv_finder_create(waterfall);

    lv_obj_set_size(finder, WIDTH, WF_HEIGHT);
    lv_obj_set_pos(finder, 0, 0);

    lv_obj_set_style_radius(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(finder, LV_OPA_0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(finder, TC(sel_bg), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(finder, LV_OPA_30, LV_PART_INDICATOR);

    lv_obj_set_style_border_width(finder, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(finder, TC(text), LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    lv_finder_clear_cursor(finder);

    /* Received text, over the lower part of the waterfall */

    text_box = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(text_box);
    lv_obj_set_pos(text_box, 13, TEXT_Y);
    lv_obj_set_size(text_box, WIDTH, TEXT_H);
    lv_obj_set_style_bg_color(text_box, TC(panel), 0);
    lv_obj_set_style_bg_opa(text_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(text_box, 1, 0);
    lv_obj_set_style_border_color(text_box, TC(dialog_border), 0);
    lv_obj_set_style_border_opa(text_box, 96, 0);
    lv_obj_set_style_pad_all(text_box, TEXT_PAD, 0);
    lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);

    txt_font = param_i_get(cfg_digi_text_compact) ? &sony_18 : &sony_24;
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

    lv_obj_add_event_cb(text_box, key_cb, LV_EVENT_KEY, NULL);

    lv_group_add_obj(keyboard_group, text_box);
    lv_group_focus_obj(text_box);
    lv_group_set_editing(keyboard_group, true);

    /* Frequency: remember where the operator was, then move to the
     * Olivia dial for whichever band is nearest. The dialog framework
     * (restore_radio) puts it all back on the way out. */
    load_band(0);

    main_screen_lock_ab(true);
    main_screen_lock_mode(true);
    main_screen_lock_freq(true);
    main_screen_lock_band(true);

    olivia_macros_load();
    refresh_macro_labels();

    knob = KNOB_FINE;
    txt_clear();
    input_active = false;
    edit_idx = -1;

    pthread_mutex_lock(&spec_mux);
    spec_ok = (psk_spec_init(&spec, (float)OLIVIA_IN_RATE) == 0);
    pthread_mutex_unlock(&spec_mux);

    alive = true;
    apply_range();

    /* Receiver and transmitter, routed to this window */

    olivia_app_set_text_cb(rx_text_cb);
    olivia_app_set_audio_tap(rx_tap);
    olivia_tx_set_echo_cb(tx_echo_cb);
    olivia_app_set_active(true);

    status_timer = lv_timer_create(status_timer_cb, STATUS_PERIOD_MS, NULL);
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
    del_timer(&rebuild_timer);
    del_timer(&focus_timer);
    del_timer(&editor_timer);
    del_timer(&knob_timer);

    knob = KNOB_FINE;
    btn_dial.mark = false;

    if (input_active) {
        textarea_window_close();
        input_active = false;
    }

    /*
     * A transmission stops at once (tx_player unkeys and gives the VFO
     * back) before the frequency is put back, or the radio would retune
     * while keyed.
     */
    olivia_tx_abort();
    for (int i = 0; i < 300 && olivia_tx_is_on(); i++)
        usleep(20000);

    /* After set_active(false) returns, neither hook runs again. */
    olivia_app_set_active(false);
    olivia_app_set_text_cb(NULL);
    olivia_app_set_audio_tap(NULL);
    olivia_tx_set_echo_cb(NULL);

    pthread_mutex_lock(&spec_mux);
    if (spec_ok)
        psk_spec_free(&spec);
    spec_ok = false;
    pthread_mutex_unlock(&spec_mux);

    waterfall = NULL;
    finder = NULL;
    text_box = NULL;
    memset(line_lbl, 0, sizeof(line_lbl));

    dsp_set_waterfall_enabled(true);
    dsp_set_spectrum_enabled(true);

    main_screen_lock_ab(false);
    main_screen_lock_mode(false);
    main_screen_lock_freq(false);
    main_screen_lock_band(false);
}
