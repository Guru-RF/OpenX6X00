/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax app: reception in Auto (APT start/stop) or
 *  Cont, live view with Align and Tilt, faxes saved as PNG in /mnt/wefax, TX of
 *  a picture. Auto Plan follows the day's charts (wefax/plan.h) and keeps the
 *  best copy of each (wefax/quality.h); the Bulletin shows each kind's newest
 *  fair copy and All Charts every chart, as tiles (wefax_bulletin.h), one of them
 *  over the whole screen and straightened there with Fix (wefax_viewer.h); from
 *  OlivLoze's fork (F4OLI), for a radio away from the internet.
 */

#include "screen_main.h"
#include "board.h"
#include "wefax_window.h"
#include "wefax_bulletin.h"
#include "wefax_viewer.h"
#include "wefax_night.h"
#include "freq_list.h"
#include "wefax/decoder.h"
#include "wefax/encoder.h"
#include "wefax/plan.h"
#include "wefax/quality.h"
#include "wefax/thumb.h"
#include "wefax/tilt.h"
#include "i18n.h"
#include "tx_player.h"
#include "msg_box.h"
#include "audio.h"
#include "mfk_control.h"
#include "vol_control.h"
#include "key_input.h"
#include "cfg/cfg_api.h"
#include "gui_styles.h"
#include "theme.h"
#include "radio.h"
#include "gui_events.h"
#include "app_frame.h"
#include "channel_list.h"
#include "remote_server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VIEW_W          750
#define VIEW_H          (APP_FRAME_BODY_H - 2)      /* the body of the common app look */
#define VIEW_H_MAX      (DIALOG_H_MAX - 30)         /* static buffers */
#define VIEW_MS         30

/* Rows kept at full size for Save: 2400 x 1809 = 4.1 MiB, a vertical chart fits */
#define STORE_ROWS      2400
#define SAVE_DIR        BOARD_DATA "/wefax"
#define TX_DIR          SAVE_DIR "/tx_pics"

typedef enum {
    VIEW_SMALL = 0,     /* low panel, the waterfall stays visible */
    VIEW_BIG,           /* common app look, the newest rows */
    VIEW_FULL           /* common app look, the whole kept fax */
} view_t;

static void app_open(lv_obj_t *parent);
static void app_close(void);
static void app_key_cb(lv_event_t *e);
static void audio_cb(unsigned int n, float *samples);

static void page_1_cb(button_data_t *data);
static void page_2_cb(button_data_t *data);
static void page_3_cb(button_data_t *data);
static void page_4_cb(button_data_t *data);
static void viewer_back_cb(button_data_t *data);
static void close_cb(button_data_t *data);
static void freq_cb(button_data_t *data);
static void rx_mode_cb(button_data_t *data);
static const char *rx_mode_label(void);
static void clear_cb(button_data_t *data);
static void align_cb(button_data_t *data);
static const char *align_label(void);
static void tilt_cb(button_data_t *data);
static const char *tilt_label(void);
static void view_cb(button_data_t *data);
static void save_cb(button_data_t *data);
static void plan_cb(button_data_t *data);
static const char *plan_label(void);
static void bulletin_cb(button_data_t *data);
static void all_charts_cb(button_data_t *data);
static void tiles_open_cb(button_data_t *data);
static void tiles_back_cb(button_data_t *data);
static void tiles_newest_cb(button_data_t *data);
static void tiles_delete_cb(button_data_t *data);
static const char *tiles_delete_label(void);
static void chart_full_cb(button_data_t *data);
static void chart_fix_cb(button_data_t *data);
static void fix_auto_cb(button_data_t *data);
static void fix_undo_cb(button_data_t *data);
static void fix_cancel_cb(button_data_t *data);
static void fix_save_cb(button_data_t *data);
static void night_cb(button_data_t *data);
static const char *night_label(void);

static void draw_live(void);
static void idle_update(void);
static void plan_stop(const char *why);
static void rx_auto(void);

/* ---- state ---- */

static wefax_decoder_t *decoder;
static uint32_t gen_seen;           /* Auto start generation already handled here */
static bool     fax_on;             /* Auto: the GUI took the confirmed fax, rows are kept */
static uint32_t fax_gen;            /* counts clear_fax: the web page starts a new picture */
static uint32_t fax_rows;           /* rows of this picture so far, past the kept ones too: the web page's row number */

/* audio thread -> LVGL thread: the last finished row */
static pthread_mutex_t row_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t  row_box[WEFAX_WIDTH];
static bool     row_full;

/* rows as decoded, Align/Tilt applied only when drawing and saving */
static uint8_t *store;
static unsigned store_n, store_head;

/* the rows on screen in Small/Big: 1 row in 1809/750, as the width */
static uint8_t  shown[VIEW_H_MAX][WEFAX_WIDTH];
static unsigned shown_n, shown_head, shown_acc;

static lv_color_t view_buf[VIEW_W * VIEW_H_MAX];
static view_t     view = VIEW_SMALL;
static lv_obj_t  *app_parent;
static lv_obj_t  *canvas, *viewport, *panel;
static app_frame_t frame;          /* header and status line, Big and Full */
static lv_timer_t *view_timer;
static bool       align_on, tilt_on; /* the encoder turns live Align / Tilt */

/* PNG written by a worker, the LVGL thread only takes a copy. The worker marks it (wefax/quality.h) and names it
 * wefax-<local date and time>[-<chart>]-q<mark>.png; of a chart of Auto Plan it keeps the best copy only */
typedef struct {
    uint8_t  *img;
    unsigned  rows;
    int       align, tilt;
    bool      no_stop;      /* not ended by its stop tone: the footer is probably cut */
    time_t    when;
    struct tm tm;           /* when, local, for the name */
    char      product[48];  /* Auto Plan: the chart, "" for none */
    char      station[16];  /* and its station */
} save_job_t;

static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static bool save_busy, save_over, save_ok;
static char save_msg[64];           /* what the overlay says once it is written */

/* TX (web remote): a worker plays the picture through tx_player, no RX meanwhile */
static pthread_t     tx_thread;
static bool          tx_thread_on;
static volatile bool tx_running, tx_abort;
static volatile int  tx_progress, tx_result;
static char          tx_path[192];

/* Auto Plan: the radio follows the day's slots in Auto (wefax/plan.h); the slot of the fax coming in names it */
static lv_timer_t *plan_timer;
static bool        plan_on;
static int32_t     plan_dial;       /* what the plan tuned: another dial is tuning by hand */
static time_t      plan_done[WEFAX_PLAN_MAX];  /* when each slot was taken or skipped */
static char        plan_btn[48];    /* the button's label: the next slot's time */
static char        plan_next[48];   /* the next slot, for the header: "HH:MM station kHz" */
static int         plan_next_i = -1;    /* and its index, for the view waiting */
static int         fax_slot = -1;   /* the slot of the fax coming in */
static time_t      fax_t0;          /* when it began */

/* Auto waiting, the view says what comes next instead of an empty picture */
static lv_obj_t   *idle, *idle_what, *idle_when, *idle_then;

/* Auto Plan turned on during a chart: the decoder takes it from where it is (wefax_decoder_auto_join), and its first
 * JOIN_ROWS rows tell a chart from noise */
#define JOIN_ROWS       30
#define JOIN_COHERENCE  0.35f       /* a chart's traces go on from a row to the next: 0.5-0.9, noise < 0.3 */
#define JOIN_DARK       0.45f       /* a chart is mostly white, noise about half dark */

static bool        join_probe;
static int         join_slot = -1;
static time_t      join_t0;         /* the start of the chart joined */


/* ---- buttons ----
 * Page 1 what the app is for off-grid, 2 reception by hand, 3 the live picture's settings and TX power, 4 the
 * charts at night. The tiles, a chart large and the full screen have their own buttons, F5 one level up there */

/* Each page button held goes to the page before (#317) */
static button_data_t btn_page_1   = {.type = BTN_TEXT, .label = "(FAX 1:4)", .press = page_2_cb, .hold = page_4_cb};
static button_data_t btn_plan     = {.type = BTN_TEXT_FN, .label_fn = plan_label, .press = plan_cb};
static button_data_t btn_bulletin = {.type = BTN_TEXT, .label = "Bulletin", .press = bulletin_cb};
static button_data_t btn_all      = {.type = BTN_TEXT, .label = "All\nCharts", .press = all_charts_cb};
static button_data_t btn_close    = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

static button_data_t btn_page_2  = {.type = BTN_TEXT, .label = "(FAX 2:4)", .press = page_3_cb, .hold = page_1_cb};
static button_data_t btn_freq    = {.type = BTN_TEXT, .label = "Freq.", .press = freq_cb};
static button_data_t btn_rx_mode = {.type = BTN_TEXT_FN, .label_fn = rx_mode_label, .press = rx_mode_cb};
static button_data_t btn_view    = {.type = BTN_TEXT, .label = "View\nBig", .press = view_cb};
static button_data_t btn_clear   = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};

static button_data_t btn_page_3 = {.type = BTN_TEXT, .label = "(FAX 3:4)", .press = page_4_cb, .hold = page_2_cb};
static button_data_t btn_align  = {.type = BTN_TEXT_FN, .label_fn = align_label, .press = align_cb,
                                   .ctrl = CTRL_WEFAX_ALIGN, .encoder_allowed = true};
static button_data_t btn_tilt   = {.type = BTN_TEXT_FN, .label_fn = tilt_label, .press = tilt_cb,
                                   .ctrl = CTRL_WEFAX_TILT, .encoder_allowed = true};
static button_data_t btn_save   = {.type = BTN_TEXT, .label = "Save", .press = save_cb};

static button_data_t btn_page_4 = {.type = BTN_TEXT, .label = "(FAX 4:4)", .press = page_1_cb, .hold = page_3_cb};
static button_data_t btn_night  = {.type = BTN_TEXT_FN, .label_fn = night_label, .press = night_cb};

static buttons_page_t page_1 = {.items = {&btn_page_1, &btn_plan, &btn_bulletin, &btn_all, &btn_close}};
static buttons_page_t page_2 = {.items = {&btn_page_2, &btn_freq, &btn_rx_mode, &btn_view, &btn_clear}};
static buttons_page_t page_3 = {.items = {&btn_page_3, &btn_align, &btn_tilt, &btn_save, &btn_dig_pwr}};
static buttons_page_t page_4 = {.items = {&btn_page_4, &btn_night, NULL, NULL, NULL}};

/* The tiles: the Bulletin's and All Charts' */
static button_data_t btn_bul_page = {.type = BTN_TEXT, .label = "(Bulletin)", .press = page_1_cb};
static button_data_t btn_all_page = {.type = BTN_TEXT, .label = "(All Charts)", .press = page_1_cb};
static button_data_t btn_open     = {.type = BTN_TEXT, .label = "Open", .press = tiles_open_cb};
static button_data_t btn_newest   = {.type = BTN_TEXT, .label = "Newest", .press = tiles_newest_cb};
static button_data_t btn_delete   = {.type = BTN_TEXT_FN, .label_fn = tiles_delete_label, .press = tiles_delete_cb};
static button_data_t btn_to_fax   = {.type = BTN_TEXT, .label = "< Fax", .press = tiles_back_cb};

static buttons_page_t page_bul = {.items = {&btn_bul_page, &btn_plan, &btn_open, &btn_all, &btn_to_fax}};
static buttons_page_t page_all = {.items = {&btn_all_page, &btn_open, &btn_newest, &btn_delete, &btn_to_fax}};

/* One chart large */
static button_data_t btn_chart_page = {.type = BTN_TEXT, .label = "(Bulletin)", .press = page_1_cb};
static button_data_t btn_full       = {.type = BTN_TEXT, .label = "Full\nScreen", .press = chart_full_cb};
static button_data_t btn_fix        = {.type = BTN_TEXT, .label = "Fix", .press = chart_fix_cb};
static button_data_t btn_to_tiles   = {.type = BTN_TEXT, .label = "< Tiles", .press = tiles_back_cb};

static buttons_page_t page_chart = {.items = {&btn_chart_page, &btn_full, &btn_fix, &btn_delete, &btn_to_tiles}};

/* The full screen viewer: its buttons under it, unseen; F3 Fix, any other back */
static button_data_t  btn_vw_back = {.type = BTN_TEXT, .label = "< Back", .press = viewer_back_cb};
static buttons_page_t page_viewer = {.items = {&btn_vw_back, &btn_vw_back, &btn_fix, &btn_vw_back, &btn_vw_back}};

/* Fix: the chart over the screen but these */
static button_data_t btn_fix_page   = {.type = BTN_TEXT, .label = "(Fix)", .press = NULL};
static button_data_t btn_fix_auto   = {.type = BTN_TEXT, .label = "Auto\nTilt", .press = fix_auto_cb};
static button_data_t btn_fix_undo   = {.type = BTN_TEXT, .label = "Undo", .press = fix_undo_cb};
static button_data_t btn_fix_cancel = {.type = BTN_TEXT, .label = "Cancel", .press = fix_cancel_cb};
static button_data_t btn_fix_save   = {.type = BTN_TEXT, .label = "Save\n& Back", .press = fix_save_cb};

static buttons_page_t page_fix = {.items = {&btn_fix_page, &btn_fix_auto, &btn_fix_undo, &btn_fix_cancel, &btn_fix_save}};

/* Frequency and mode come back when the app closes */
static dialog_t dialog = {
    .construct_cb = app_open, .destruct_cb = app_close, .audio_cb = audio_cb, .key_cb = app_key_cb,
    .btn_page = &page_1, .restore_radio = true
};

dialog_t *dialog_wefax = &dialog;

bool dialog_wefax_is_active(void)
{
    return dialog.run;
}

/* Stations with the decoder's format (120 LPM, IOC 576); the decoder is centred on 1500 Hz */
static const freq_list_item_t stations[] = {
    {.group = "DWD (DE)"},
    {.freq_hz = 3855000, .name = "DDH3", .note = "night"},
    {.freq_hz = 7880000, .name = "DDK3", .note = "day and night"},
    {.freq_hz = 13882500, .name = "DDK6", .note = "day"},
    {.group = "Northwood (UK)"},
    {.freq_hz = 2618500, .name = "GYA"},
    {.freq_hz = 4610000, .name = "GYA"},
    {.freq_hz = 8040000, .name = "GYA"},
    {.freq_hz = 11086500, .name = "GYA"},
    {.group = "Athens (GR)"},
    {.freq_hz = 4481000, .name = "SVJ4"},
    {.freq_hz = 8105000, .name = "SVJ4"},
    /* NOAA, the assigned frequencies of its radiofax schedules */
    {.group = "Boston (US)"},
    {.freq_hz = 4235000, .name = "NMF", .note = "night"},
    {.freq_hz = 6340500, .name = "NMF"},
    {.freq_hz = 9110000, .name = "NMF"},
    {.freq_hz = 12750000, .name = "NMF", .note = "day"},
    {.group = "New Orleans (US)"},
    {.freq_hz = 4317900, .name = "NMG", .note = "night"},
    {.freq_hz = 8503900, .name = "NMG"},
    {.freq_hz = 12789900, .name = "NMG"},
    {.freq_hz = 17146400, .name = "NMG", .note = "day"},
    {.group = "Pt. Reyes (US)"},
    {.freq_hz = 4346000, .name = "NMC", .note = "night"},
    {.freq_hz = 8682000, .name = "NMC"},
    {.freq_hz = 12786000, .name = "NMC"},
    {.freq_hz = 17151200, .name = "NMC", .note = "day"},
    {.freq_hz = 22527000, .name = "NMC", .note = "day"},
    {.group = "Kodiak (US)"},
    {.freq_hz = 2054000, .name = "NOJ", .note = "night"},
    {.freq_hz = 4298000, .name = "NOJ"},
    {.freq_hz = 8459000, .name = "NOJ"},
    {.freq_hz = 12412500, .name = "NOJ", .note = "day"},
    {.group = "Honolulu (US)"},
    {.freq_hz = 9982500, .name = "KVM70"},
    {.freq_hz = 11090000, .name = "KVM70"},
    {.freq_hz = 16135000, .name = "KVM70"},
    {.freq_hz = 23331500, .name = "KVM70", .note = "day"},
};

#define STATIONS_N  ((int)(sizeof(stations) / sizeof(stations[0])))

/* A frequency this close to a station is that station, in the list and in the header (the channel
 * list's WEFAX DWD DDK3 is 30 Hz off) */
#define SAME_STATION_HZ 50

/* What Freq. lists and the header names: the stations, then under a row of their own the
 * channels whose name starts with WEFAX (APP > Channels, the web Memories page). A channel
 * holds the dial, so its station is the dial + the audio centre; one at a station's frequency
 * (the WEFAX channels a new radio starts with) is that station and is not listed twice. */
static freq_list_item_t freqs[STATIONS_N + 1 + CHANNELS_MAX];
static char             freq_names[CHANNELS_MAX][CHANNEL_NAME_MAX];
static int              freqs_n;

/* The entry nearest to freq_hz (a station frequency) within SAME_STATION_HZ, NULL for none */
static const freq_list_item_t *entry_at(const freq_list_item_t *list, int n, int32_t freq_hz)
{
    const freq_list_item_t *best = NULL;

    for (int i = 0; i < n; i++)
        if (!list[i].group && abs(list[i].freq_hz - freq_hz) <= SAME_STATION_HZ &&
            (!best || abs(list[i].freq_hz - freq_hz) < abs(best->freq_hz - freq_hz)))
            best = &list[i];
    return best;
}

/* A channel's name as listed: what follows WEFAX (all of it when nothing does), no '#' (it
 * would break the header's colours). '#' is skipped with the separators, so "WEFAX #" is not
 * listed with an empty name */
static void channel_name(char *out, size_t size, const char *name)
{
    const char *p = name + 5;
    size_t      o = 0;

    while (*p == ' ' || *p == '-' || *p == '_' || *p == ':' || *p == '.' || *p == '/' || *p == '#')
        p++;
    if (!*p)
        p = name;
    for (; *p && o + 1 < size; p++)
        if (*p != '#')
            out[o++] = *p;
    out[o] = '\0';
}

static void freqs_build(void)
{
    int named = 0;

    memcpy(freqs, stations, sizeof(stations));
    freqs_n = STATIONS_N;

    channels_reload_if_changed();       /* one added on the web a moment ago */
    for (uint16_t i = 0; i < channels_count(); i++) {
        const channel_t *c = channels_get(i);

        if (strncasecmp(c->name, "WEFAX", 5) || c->freq > INT32_MAX - WEFAX_CENTER_HZ)
            continue;

        int32_t f = (int32_t)c->freq + WEFAX_CENTER_HZ;     /* USB */

        if (entry_at(stations, STATIONS_N, f))
            continue;
        if (!named)
            freqs[freqs_n++] = (freq_list_item_t){.group = "Channels"};
        channel_name(freq_names[named], sizeof(freq_names[named]), c->name);
        freqs[freqs_n++] = (freq_list_item_t){.freq_hz = f, .name = freq_names[named]};
        named++;
    }
}

static bool auto_mode(void)
{
    return decoder && wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO;
}

/* ---- geometry of the corrections ---- */

static int wrap_x(int x, int width)
{
    while (x < 0)
        x += width;
    while (x >= width)
        x -= width;
    return x;
}

/* Tilt is in 1/1000 px per row of the 750 px view; on a picture of the given width
 * it scales with the rows. The newest row does not move. */
_Static_assert(VIEW_W == WEFAX_TILT_UNIT_W, "the tilt's unit is a row of the view");

static int tilt_px(int64_t from_newest, int tilt, unsigned width)
{
    return (int)(from_newest * tilt * VIEW_W / (1000LL * width));
}

static void fill_view(lv_color_t c)
{
    for (unsigned i = 0; i < VIEW_W * VIEW_H; i++)
        view_buf[i] = c;
}

static bool night_now;      /* the drawing under way dims its rows (wefax_night.h) */

static void put_px(unsigned x, unsigned y, uint8_t g)
{
    g = wefax_night_grey(g, night_now);
    view_buf[y * VIEW_W + x] = lv_color_make(g, g, g);
}

/* ---- kept rows ---- */

static const uint8_t *store_row(unsigned i)     /* 0 = oldest */
{
    return store + (size_t)((store_head + STORE_ROWS - store_n + i) % STORE_ROWS) * WEFAX_WIDTH;
}

static void store_add(const uint8_t *row)
{
    if (!store) {
        store = malloc((size_t)STORE_ROWS * WEFAX_WIDTH);
        if (!store) {
            printf("WEFAX: no memory for %u rows\n", STORE_ROWS);
            return;
        }
        store_n = store_head = 0;
    }
    memcpy(store + (size_t)store_head * WEFAX_WIDTH, row, WEFAX_WIDTH);
    store_head = (store_head + 1) % STORE_ROWS;
    if (store_n < STORE_ROWS)
        store_n++;
}

/* Empty view, kept rows and the row in the box. The picture's empty part is the panel's colour: waiting it says what
 * comes next in the theme's text, and a white page at night dazzles */
static void clear_fax(void)
{
    fill_view(TC(panel));
    shown_acc = shown_n = shown_head = 0;
    if (canvas)
        lv_obj_invalidate(canvas);

    pthread_mutex_lock(&row_lock);
    row_full = false;
    pthread_mutex_unlock(&row_lock);

    store_n = store_head = 0;
    fax_rows = 0;
    fax_gen++;
    fax_slot = -1;
    idle_update();
}

/* ---- drawing ---- */

/* Full: every kept row, fitted in the body, newest at the bottom */
static void draw_full(void)
{
    if (!canvas)
        return;

    night_now = wefax_night();
    fill_view(TC(panel));
    if (!store || !store_n) {
        lv_obj_invalidate(canvas);
        return;
    }

    int      align = (int)param_i_get(cfg_wefax_align);
    int      tilt  = (int)param_i_get(cfg_wefax_tilt);
    unsigned h     = VIEW_H;
    unsigned w     = (unsigned)((uint64_t)WEFAX_WIDTH * h / store_n);

    if (w > VIEW_W) {
        w = VIEW_W;
        h = (unsigned)((uint64_t)store_n * w / WEFAX_WIDTH);
    }
    if (!w || !h) {
        lv_obj_invalidate(canvas);
        return;
    }

    unsigned x0   = (VIEW_W - w) / 2, y0 = VIEW_H - h;
    unsigned need = (unsigned)(((uint64_t)h * WEFAX_WIDTH + w - 1) / w);

    if (need > store_n)
        need = store_n;
    for (unsigned y = 0; y < h; y++) {
        unsigned r = store_n - need + (unsigned)((uint64_t)y * need / h);

        if (r >= store_n)
            r = store_n - 1;

        const uint8_t *src   = store_row(r);
        int            shift = tilt_px((int64_t)r - (int64_t)(store_n - 1), tilt, WEFAX_WIDTH);

        for (unsigned x = 0; x < w; x++)
            put_px(x0 + x, y0 + y, src[wrap_x((int)((uint64_t)x * WEFAX_WIDTH / w) - align - shift, WEFAX_WIDTH)]);
    }
    lv_obj_invalidate(canvas);
}

/* Small/Big: the rows on screen, 1809 -> 750 px by averaging, newest at the bottom */
static void draw_live(void)
{
    if (view == VIEW_FULL) {
        draw_full();
        return;
    }

    int      align = (int)param_i_get(cfg_wefax_align);
    int      tilt  = (int)param_i_get(cfg_wefax_tilt);     /* 1/1000 px per view row */
    unsigned first = (shown_head + VIEW_H - shown_n) % VIEW_H;
    unsigned y0    = VIEW_H - shown_n;

    night_now = wefax_night();
    fill_view(TC(panel));
    for (unsigned r = 0; r < shown_n; r++) {
        const uint8_t *src   = shown[(first + r) % VIEW_H];
        int            shift = ((int)r - (int)(shown_n - 1)) * tilt / 1000;

        for (unsigned x = 0; x < VIEW_W; x++) {
            unsigned from = x * WEFAX_WIDTH / VIEW_W, to = (x + 1) * WEFAX_WIDTH / VIEW_W, sum = 0;

            if (to <= from)
                to = from + 1;
            for (unsigned sx = from; sx < to; sx++)
                sum += src[wrap_x((int)sx - align - shift, WEFAX_WIDTH)];
            put_px(x, y0 + r, (uint8_t)(sum / (to - from)));
        }
    }
    if (canvas)
        lv_obj_invalidate(canvas);
}

/* ---- PNG ---- */

/* A worse copy of a chart removed: its preview goes with it */
static void copy_dropped(const char *dir, const char *name)
{
    wefax_preview_remove(dir, name);
}

/* ---- what the browser's WeFax page sees (wefax_window_status) ---- */

static pthread_mutex_t rstat_lock = PTHREAD_MUTEX_INITIALIZER;
static wefax_status_t  rstat;       /* under rstat_lock: the LVGL thread writes it, the remote server's thread reads */

/* LVGL thread: the state as it is now */
static void rstat_update(void)
{
    int32_t                 f  = cparam_i_get(cfg_fg_freq) + WEFAX_CENTER_HZ;
    const freq_list_item_t *at = entry_at(freqs, freqs_n, f);

    pthread_mutex_lock(&rstat_lock);
    rstat.open      = decoder != NULL;
    rstat.auto_mode = decoder && auto_mode();
    rstat.fax_on    = decoder && (rstat.auto_mode ? fax_on : store_n > 0);
    rstat.tx        = tx_running;
    rstat.tx_pct    = tx_progress / 10;
    rstat.rows      = store_n;
    rstat.gen       = fax_gen;
    rstat.freq      = f;
    snprintf(rstat.station, sizeof(rstat.station), "%s", at && at->name ? at->name : "");
    rstat.plan = decoder && plan_on;
    snprintf(rstat.plan_next, sizeof(rstat.plan_next), "%s", rstat.plan ? plan_next : "");
    pthread_mutex_unlock(&rstat_lock);
}

/* The save worker: a picture written, its file name for the page's Last charts */
static void rstat_saved(const char *path)
{
    const char *base = strrchr(path, '/');

    pthread_mutex_lock(&rstat_lock);
    snprintf(rstat.saved, sizeof(rstat.saved), "%s", base ? base + 1 : path);
    rstat.saves++;
    pthread_mutex_unlock(&rstat_lock);
}

void wefax_window_status(wefax_status_t *s)
{
    pthread_mutex_lock(&rstat_lock);
    *s = rstat;
    pthread_mutex_unlock(&rstat_lock);
}

static void *save_worker(void *arg)
{
    save_job_t *job = arg;
    bool        ok  = false, dropped = false;
    char        name[128], path[sizeof(name) + 32], msg[sizeof(save_msg)];
    int         q    = (int)lroundf(wefax_quality(job->img, WEFAX_WIDTH, job->rows, job->no_stop, NULL));
    bool        same = job->product[0] && !wefax_plan_product_unique(job->product);
    int         best = same ? wefax_plan_copies(SAVE_DIR, job->product, job->when, NULL, false, NULL) : -1;

    /* Auto Plan's chart straightened, its station's tilt learned: a clean copy only (60 and up), noise has no lines
     * to go by */
    int t;

    if (job->product[0] && q >= 60 && best < q && wefax_auto_tilt(job->img, WEFAX_WIDTH, job->rows, &t)) {
        if (t != job->tilt)
            printf("WEFAX TILT: %s straightened, tilt %+d (was %+d)\n", job->product, t, job->tilt);
        job->tilt = t;
        wefax_station_tilt_learn(job->station, t);
    }

    wefax_plan_file_name(name, sizeof(name), &job->tm, job->product, q);
    snprintf(path, sizeof(path), SAVE_DIR "/%s", name);
    snprintf(msg, sizeof(msg), _("Fax saved, quality %d"), q);

    if (best >= q) {
        /* a copy of this chart as good or better is there already */
        ok = dropped = true;
        snprintf(msg, sizeof(msg), _("Fax dropped: quality %d, a copy has %d"), q, best);
        printf("WEFAX PLAN: %s quality %d dropped, a copy has %d\n", job->product, q, best);
    } else if (mkdir(SAVE_DIR, 0775) != 0 && errno != EEXIST) {
        printf("WEFAX: can't make %s: %s\n", SAVE_DIR, strerror(errno));
    } else {
        /* written aside, then renamed: what reads the folder (the tiles, the web page) never gets half a fax */
        ok = wefax_png_write_fixed(path, job->img, WEFAX_WIDTH, job->rows, job->align, job->tilt);
    }

    if (ok && !dropped) {
        printf("WEFAX: saved %s (%u x %u, quality %d)\n", path, WEFAX_WIDTH, job->rows, q);
        rstat_saved(path);
        if (best >= 0)
            wefax_plan_copies(SAVE_DIR, job->product, job->when, name, true, copy_dropped);   /* the worse go */
        wefax_preview_make(SAVE_DIR, name);         /* for the tiles */
    }

    free(job->img);
    free(job);

    pthread_mutex_lock(&save_lock);
    save_busy = false;
    save_ok   = ok;
    save_over = true;
    snprintf(save_msg, sizeof(save_msg), "%s", msg);
    pthread_mutex_unlock(&save_lock);
    return NULL;
}

/* Copy the kept fax (oldest row first) and hand it to a worker; the kept rows can go right after. no_stop: it was not
 * ended by its stop tone */
static bool save_fax(bool no_stop)
{
    if (!store || !store_n) {
        printf("WEFAX: nothing to save\n");
        return false;
    }

    pthread_mutex_lock(&save_lock);
    bool busy = save_busy;
    pthread_mutex_unlock(&save_lock);

    if (busy) {
        printf("WEFAX: the last save is still running\n");
        return false;
    }

    save_job_t *job = calloc(1, sizeof(*job));
    time_t      now = time(NULL);

    if (!job || !(job->img = malloc((size_t)store_n * WEFAX_WIDTH)) || !localtime_r(&now, &job->tm)) {
        printf("WEFAX: can't save, no memory or clock\n");
        if (job)
            free(job->img);
        free(job);
        return false;
    }
    job->rows    = store_n;
    job->no_stop = no_stop;
    job->when    = now;
    job->align   = (int)param_i_get(cfg_wefax_align);
    job->tilt    = (int)param_i_get(cfg_wefax_tilt);
    if (plan_on && fax_on && wefax_plan_slot(fax_slot)) {
        snprintf(job->product, sizeof(job->product), "%s", wefax_plan_slot(fax_slot)->product);
        snprintf(job->station, sizeof(job->station), "%s", wefax_plan_slot(fax_slot)->station);
    }
    for (unsigned r = 0; r < store_n; r++)
        memcpy(job->img + (size_t)r * WEFAX_WIDTH, store_row(r), WEFAX_WIDTH);

    pthread_t th;

    pthread_mutex_lock(&save_lock);
    save_busy = true;
    save_over = false;
    save_ok   = false;
    pthread_mutex_unlock(&save_lock);

    int rc = pthread_create(&th, NULL, save_worker, job);

    if (rc) {
        pthread_mutex_lock(&save_lock);
        save_busy = false;
        pthread_mutex_unlock(&save_lock);
        printf("WEFAX: can't start the save: %s\n", strerror(rc));
        free(job->img);
        free(job);
        return false;
    }
    pthread_detach(th);
    channel_overlay_show("Saving fax...");
    return true;
}

static void save_result(void)
{
    char msg[sizeof(save_msg)];

    pthread_mutex_lock(&save_lock);
    bool over = save_over, ok = save_ok;
    save_over = false;
    snprintf(msg, sizeof(msg), "%s", save_msg);
    pthread_mutex_unlock(&save_lock);

    if (over)
        channel_overlay_show(ok ? msg : "Fax save failed");
}

/* ---- Auto ---- */

/* A fax ended by itself (its stop tone, a new start, a slot of the plan) is saved unless it is a few rows only: a
 * false start, noise taken for a start tone (a chart is about 1300 rows). False: it could not be saved */
#define AUTO_MIN_ROWS   200

static bool save_auto(bool no_stop)
{
    if (store_n && store_n < AUTO_MIN_ROWS) {
        printf("WEFAX AUTO: %u rows only, a false start: not saved\n", store_n);
        return true;
    }
    return !store_n || save_fax(no_stop);
}

/* A new start over a fax the GUI took: the old one is over even if its stop was missed.
 * The decoder already follows the new fax, it is not reset. */
static bool auto_keep_previous(void)
{
    if (!save_auto(true)) {
        printf("WEFAX AUTO: the previous fax could not be saved, kept\n");
        return false;
    }
    clear_fax();
    fax_on = false;
    return true;
}

/* Stop tone (stopped), kept rows full, or Auto Plan moving on: save, then Auto waits for the next start */
static void auto_fax_over(bool stopped)
{
    if (!decoder)
        return;
    if (!save_auto(!stopped)) {
        printf("WEFAX AUTO: the fax could not be saved, kept\n");
        return;
    }
    gen_seen   = 0;
    fax_on     = false;
    join_probe = false;
    wefax_decoder_set_mode(decoder, WEFAX_MODE_AUTO);      /* one reset only: it keeps the pause after the stop */
    clear_fax();
}

/* The decoder starts over (Clear, after a TX) */
static void rx_restart(void)
{
    wefax_decoder_reset(decoder);
    gen_seen   = 0;
    fax_on     = false;
    join_probe = false;
}

/* ---- audio thread ---- */

static void audio_cb(unsigned int n, float *samples)
{
    if (decoder && !tx_running)
        wefax_decoder_process(decoder, samples, n);
}

/* A finished row; in Auto only once the fax is confirmed. No LVGL here. */
static void row_cb(const uint8_t *row, unsigned int width, unsigned int row_number, void *user_data)
{
    (void)row_number;
    (void)user_data;

    if (!row || width != WEFAX_WIDTH || (auto_mode() && !wefax_decoder_auto_fax_confirmed(decoder)))
        return;

    pthread_mutex_lock(&row_lock);
    memcpy(row_box, row, WEFAX_WIDTH);
    row_full = true;
    pthread_mutex_unlock(&row_lock);
}

/* ---- TX ---- */

static bool tx_abort_cb(void *ctx)
{
    (void)ctx;
    return tx_abort;
}

static size_t tx_fill_cb(int16_t *buf, size_t n, void *enc)
{
    n           = wefax_encoder_fill(buf, n, enc);
    tx_progress = wefax_encoder_progress(enc);
    return n;
}

static void *tx_worker(void *arg)
{
    (void)arg;

    int              rc;
    wefax_encoder_t *enc = wefax_encoder_open(tx_path, AUDIO_PLAY_RATE, &rc);

    if (!enc) {
        printf("WEFAX TX: can't load %s (%d)\n", tx_path, rc);
        tx_result  = rc;
        tx_running = false;
        return NULL;
    }
    printf("WEFAX TX: %s, %u lines\n", tx_path, wefax_encoder_rows(enc));

    /* absolute audio tones as SSTV: offset = tx_player's centre, no VFO shift */
    bool done = tx_player_stream(tx_fill_cb, enc, TX_PLAYER_AUDIO_HZ, tx_player_base_gain_offset(), tx_abort_cb, NULL);

    wefax_encoder_close(enc);
    tx_result  = done && !tx_abort ? 1 : -1;
    tx_running = false;
    return NULL;
}

/* name: a file of TX_DIR, [A-Za-z0-9][A-Za-z0-9._ -]*.png */
static void tx_start(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    bool   ok  = len > 4 && len < 128 && isalnum((unsigned char)name[0]) && !strcmp(name + len - 4, ".png");

    for (size_t i = 0; ok && i < len; i++)
        ok = isalnum((unsigned char)name[i]) || strchr("._ -", name[i]);
    if (!ok) {
        msg_update_text_fmt("WeFax TX: bad file name");
        return;
    }
    if (tx_running || tx_thread_on) {
        msg_update_text_fmt("WeFax TX busy");
        return;
    }

    snprintf(tx_path, sizeof(tx_path), TX_DIR "/%s", name);
    tx_abort    = false;
    tx_progress = 0;
    tx_result   = 0;
    tx_running  = true;

    int rc = pthread_create(&tx_thread, NULL, tx_worker, NULL);

    if (rc) {
        printf("WEFAX TX: can't start: %s\n", strerror(rc));
        tx_running = false;
        return;
    }
    tx_thread_on = true;
}

/* LVGL thread: progress bar, and the end of a TX */
static void tx_poll(void)
{
    if (tx_running) {
        app_frame_bar(&frame, tx_progress, theme_hex(TC_tx));
        return;
    }
    if (!tx_thread_on)
        return;

    pthread_join(tx_thread, NULL);
    tx_thread_on = false;
    app_frame_bar(&frame, -1, 0);

    if (tx_result == 1)
        msg_update_text_fmt("WeFax TX done");
    else if (tx_result == -1)
        msg_update_text_fmt("WeFax TX stopped");
    else
        msg_update_text_fmt("WeFax TX failed (%i)", tx_result);

    if (decoder)
        rx_restart();       /* RX goes on from a clean decoder */
}

/* ---- Auto Plan ---- */

static const char *plan_label(void)
{
    return plan_btn[0] ? plan_btn : "Auto\nPlan";
}

/* The button, the header and the view waiting name the next slot. "Plan" over "next HH:MM": two lines in every
 * theme ("Auto Plan" over it took three) */
static void plan_show(int next)
{
    const wefax_slot_t *s = wefax_plan_slot(next);
    char                l[sizeof(plan_btn)];

    if (!plan_on)
        snprintf(l, sizeof(l), "%s", "Auto\nPlan");
    else if (!s)
        snprintf(l, sizeof(l), "%s", _("Plan\nnext --:--"));
    else
        snprintf(l, sizeof(l), _("Plan\nnext %02d:%02d"), s->min / 60, s->min % 60);
    plan_next_i = plan_on && s ? next : -1;
    if (s && plan_on)
        snprintf(plan_next, sizeof(plan_next), "%02d:%02d %s %.1f", s->min / 60, s->min % 60, s->station,
                 s->freq_hz / 1000.0);
    else
        plan_next[0] = '\0';
    if (strcmp(l, plan_btn)) {
        snprintf(plan_btn, sizeof(plan_btn), "%s", l);
        if (btn_plan.disp_btn)
            buttons_refresh(&btn_plan);
    }
    idle_update();
}

/* The slot's station in USB and RX Auto: the plan's tuning; another dial from now on is tuning by hand */
static void plan_tune(const wefax_slot_t *s)
{
    freq_list_item_t it   = {.freq_hz = s->freq_hz, .name = s->station};
    int32_t          dial = s->freq_hz - WEFAX_CENTER_HZ;

    if (cparam_i_get(cfg_fg_freq) != dial || cparam_i_get(cfg_cur_mode) != radio_mode_usb)
        freq_list_tune(&it, WEFAX_CENTER_HZ);
    plan_dial = dial;
    if (!auto_mode()) {
        save_auto(true);
        rx_auto();
        buttons_refresh(&btn_rx_mode);
    }
    printf("WEFAX PLAN: %02d:%02d %s %.1f kHz %s\n", s->min / 60, s->min % 60, s->station, s->freq_hz / 1000.0,
           s->product);

    /* the station's own tilt, learned from its charts: they come in straight */
    int tilt;

    if (wefax_station_tilt(s->station, &tilt) && tilt != (int)param_i_get(cfg_wefax_tilt)) {
        param_i_set(cfg_wefax_tilt, tilt);
        if (btn_tilt.disp_btn)
            buttons_refresh(&btn_tilt);
        printf("WEFAX TILT: %s, tilt %+d\n", s->station, tilt);
    }
}

/* A slot's time: a fax of the plan coming in keeps the radio, any other gives way (saved as it is) */
static void plan_take(int i, time_t now)
{
    const wefax_slot_t *s = wefax_plan_slot(i);

    if (fax_on) {
        if (wefax_plan_keeps_radio(wefax_plan_slot(fax_slot), (int)(now - fax_t0), s)) {
            printf("WEFAX PLAN: %02d:%02d %s skipped, a fax is coming in\n", s->min / 60, s->min % 60, s->product);
            return;
        }
        auto_fax_over(false);
    }
    plan_tune(s);
}

/* Each second: tuned by hand stops the plan; the next slot is taken WEFAX_PLAN_EARLY_S before its start */
static void plan_timer_cb(lv_timer_t *t)
{
    (void)t;
    idle_update();          /* "in 19 min", a TX over */

    static const char *night_shown;     /* Auto turns on at dusk: the button follows */

    if (btn_night.disp_btn && wefax_night_label() != night_shown) {
        night_shown = wefax_night_label();
        buttons_refresh(&btn_night);
    }
    if (!plan_on || !decoder || tx_running)
        return;
    if (plan_dial && cparam_i_get(cfg_fg_freq) != plan_dial) {
        plan_stop("Auto Plan off: tuned by hand");
        return;
    }

    time_t now  = time(NULL);
    int    sod  = wefax_plan_sod(now), wait;
    int    next = wefax_plan_next(sod, now, plan_done, &wait);

    if (next >= 0 && wait <= 0) {
        plan_done[next] = now;
        plan_take(next, now);
        next = wefax_plan_next(sod, now, plan_done, &wait);     /* the button names the one after */
    }
    plan_show(next);
}

static void plan_start(void)
{
    if (!decoder || plan_on)
        return;
    if (!wefax_plan_load()) {
        channel_overlay_show("Auto Plan: no plan");
        return;
    }
    memset(plan_done, 0, sizeof(plan_done));
    plan_on   = true;
    plan_dial = 0;
    param_i_set(cfg_wefax_plan, 1);         /* on again when the app opens */
    buttons_mark(&btn_plan, true);
    channel_overlay_show(wefax_plan_from_file() ? "Auto Plan on (plan.json)" : "Auto Plan on");
    printf("WEFAX PLAN: on, %d slots\n", wefax_plan_count());
    plan_timer_cb(NULL);

    /* Between two slots: the station of the last one that started, Auto waits for the next start. During its chart,
     * the chart from where it is: off-grid the app is opened when it is needed, not before a slot */
    time_t now = time(NULL);
    int    since, last = wefax_plan_last_started(wefax_plan_sod(now), &since);

    if (plan_dial || last < 0 || fax_on)
        return;

    const wefax_slot_t *s = wefax_plan_slot(last);

    plan_tune(s);
    if (wefax_plan_worth_joining(s, since)) {
        join_slot  = last;
        join_t0    = now - since;
        join_probe = true;
        wefax_decoder_auto_join(decoder);
        printf("WEFAX PLAN: %02d:%02d %s started %d s ago, taken from here\n", s->min / 60, s->min % 60, s->product,
               since);
    }
}

static void plan_stop(const char *why)
{
    if (!plan_on)
        return;
    plan_on    = false;
    plan_dial  = 0;
    join_probe = false;
    buttons_mark(&btn_plan, false);
    plan_show(-1);
    if (why)
        channel_overlay_show(why);
    printf("WEFAX PLAN: off%s%s\n", why ? ", " : "", why ? why : "");
}

/* Off by its button: off when the app opens too. Tuning by hand stops it only until then */
static void plan_off(void)
{
    plan_stop("Auto Plan off");
    param_i_set(cfg_wefax_plan, 0);
}

static void plan_cb(button_data_t *data)
{
    (void)data;
    if (plan_on)
        plan_off();
    else
        plan_start();
}

/* ---- LVGL timer ---- */

/* Station and frequency on the left, receiver state on the right */
static void header_update(void)
{
    char                    l[64], r[48];
    int32_t                 f  = cparam_i_get(cfg_fg_freq) + WEFAX_CENTER_HZ;     /* USB */
    const freq_list_item_t *at = entry_at(freqs, freqs_n, f);

    snprintf(l, sizeof(l), "%s   %.1f kHz", at ? at->name : "WeFax", f / 1000.0);
    const char             *rx = decoder && wefax_decoder_get_mode(decoder) == WEFAX_MODE_CONT ? "RX Cont" : "RX Auto";

    if (tx_running)
        snprintf(r, sizeof(r), "#%06x TX %i%%#", theme_hex(TC_tx), tx_progress / 10);
    else if (plan_on && plan_next[0])
        snprintf(r, sizeof(r), "%s   %s %.5s", _(rx), _("Plan"), plan_next);     /* the next slot's time */
    else
        snprintf(r, sizeof(r), "%s", rx);

    if (strcmp(l, lv_label_get_text(frame.head_left)))
        lv_label_set_text(frame.head_left, l);
    if (strcmp(r, lv_label_get_text(frame.head_right)))
        lv_label_set_text(frame.head_right, r);
}

/* "in 19 min", "in 2 h 05", "now" */
static void wait_text(char *out, size_t n, int wait)
{
    if (wait < 60)
        snprintf(out, n, "%s", _("now"));
    else if (wait < 3600)
        snprintf(out, n, _("in %d min"), wait / 60);
    else
        snprintf(out, n, _("in %d h %02d"), wait / 3600, wait % 3600 / 60);
}

/* Auto waiting in the Big or Full view, the tiles away: the chart Auto Plan takes next, its station and frequency,
 * when, and the two after it; Auto Plan off, how a fax comes in */
static void idle_update(void)
{
    if (!idle)
        return;

    bool show = decoder && auto_mode() && !store_n && !tx_running && view != VIEW_SMALL && !wefax_bulletin_is_open();

    if (!show) {
        lv_obj_add_flag(idle, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(idle, LV_OBJ_FLAG_HIDDEN);

    const wefax_slot_t *s = plan_on ? wefax_plan_slot(plan_next_i) : NULL;

    if (!s) {
        lv_label_set_text(idle_what, plan_on ? "No chart planned" : "Auto Plan is off");
        lv_label_set_text(idle_when, "a fax starts by itself with its start tone");
        lv_label_set_text(idle_then, plan_on ? "" : "Auto Plan (F2 of page 1) takes the day's charts by itself");
        return;
    }

    char   name[48], when[32], then[160] = "", one[48];
    int    sod  = wefax_plan_sod(time(NULL));
    int    wait = ((s->min * 60 - sod) % 86400 + 86400) % 86400;
    int    slot[6], waits[6], n = wefax_plan_coming(NULL, NULL, sod, slot, waits, 6), k = 0;

    wefax_bulletin_product_name(s->product, name, sizeof(name));
    lv_label_set_text_fmt(idle_what, "%s  -  %s %.1f kHz", name, s->station, s->freq_hz / 1000.0);
    wait_text(when, sizeof(when), wait);
    lv_label_set_text_fmt(idle_when, "%02d:%02d UTC, %s", s->min / 60, s->min % 60, when);

    /* the two after it */
    for (int i = 0; i < n && k < 2; i++) {
        const wefax_slot_t *o = wefax_plan_slot(slot[i]);

        if (waits[i] <= wait || !o)
            continue;
        wefax_bulletin_product_name(o->product, one, sizeof(one));
        snprintf(then + strlen(then), sizeof(then) - strlen(then), k ? "   %02d:%02d %s (%s)" : _("then %02d:%02d %s (%s)"),
                 o->min / 60, o->min % 60, one, o->station);
        k++;
    }
    lv_label_set_text(idle_then, then);
}

static lv_obj_t *idle_label(const lv_font_t *font, lv_color_t color, int y)
{
    lv_obj_t *l = lv_label_create(idle);

    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(l, VIEW_W - 40);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

static void idle_create(void)
{
    idle = lv_obj_create(viewport);
    lv_obj_remove_style_all(idle);
    lv_obj_set_size(idle, VIEW_W - 40, 200);
    lv_obj_align(idle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(idle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(idle, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(idle_label(&sony_22, TC(text_soft), 6), "Waiting for the next chart");
    idle_what = idle_label(&sony_28, TC(text), 48);
    idle_when = idle_label(&sony_24, TC(text_head), 94);
    idle_then = idle_label(&sony_18, TC(text_soft), 150);
    lv_obj_add_flag(idle, LV_OBJ_FLAG_HIDDEN);
}

static void view_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (idle && store_n && !lv_obj_has_flag(idle, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(idle, LV_OBJ_FLAG_HIDDEN);      /* the chart coming in shows at once */


    if (frame.head_left)
        header_update();
    rstat_update();
    tx_poll();
    save_result();
    if (!canvas)
        return;

    if (auto_mode()) {
        /* a new start: a fax the GUI took is saved first */
        uint32_t gen = wefax_decoder_auto_start_generation(decoder);

        if (gen != gen_seen) {
            if (fax_on) {
                if (!auto_keep_previous())
                    return;
            } else {
                clear_fax();
            }
            gen_seen = gen;
        }

        /* confirmed by the black band: from here the rows are shown and kept */
        if (wefax_decoder_auto_fax_confirmed(decoder) && !fax_on) {
            fax_on = true;
            clear_fax();
            fax_t0   = time(NULL);
            fax_slot = plan_on ? wefax_plan_slot_of_fax(cparam_i_get(cfg_fg_freq) + WEFAX_CENTER_HZ,
                                                        wefax_plan_sod(fax_t0)) : -1;
            if (join_probe) {
                fax_t0   = join_t0;
                fax_slot = join_slot;
            }
            channel_overlay_show(join_probe ? "Taking the fax going on" : "Fax Started");
        }

        /* the stop tone: file I/O here, never in the audio thread */
        if (wefax_decoder_auto_stop_detected(decoder)) {
            auto_fax_over(true);
            return;
        }
    }

    uint8_t row[WEFAX_WIDTH];

    pthread_mutex_lock(&row_lock);
    bool got = row_full;

    if (got)
        memcpy(row, row_box, WEFAX_WIDTH);
    row_full = false;
    pthread_mutex_unlock(&row_lock);
    if (!got)
        return;

    store_add(row);
    remote_server_wefax_row(row, WEFAX_WIDTH, ++fax_rows);

    /* a chart joined: its first rows say whether there is one */
    if (join_probe && fax_on && store_n >= JOIN_ROWS) {
        uint8_t *img  = malloc((size_t)JOIN_ROWS * WEFAX_WIDTH);
        float    dark = 1.0f, c = 0.0f;

        join_probe = false;
        if (img) {
            for (unsigned r = 0; r < JOIN_ROWS; r++)
                memcpy(img + (size_t)r * WEFAX_WIDTH, store_row(store_n - JOIN_ROWS + r), WEFAX_WIDTH);
            c = wefax_quality_rows(img, WEFAX_WIDTH, JOIN_ROWS, &dark);
            free(img);
        }
        printf("WEFAX PLAN: joined rows: coherence %.2f, dark %.2f\n", c, dark);
        if (c < JOIN_COHERENCE || dark > JOIN_DARK) {
            channel_overlay_show("No fax going on, waiting for the next");
            rx_restart();
            clear_fax();
            return;
        }
        channel_overlay_show("Fax going on: taken from here");
    }

    /* a fax longer than the kept rows: save it rather than lose its top */
    if (auto_mode() && fax_on && store_n >= STORE_ROWS) {
        printf("WEFAX AUTO: %u rows kept, saving\n", store_n);
        auto_fax_over(false);
        return;
    }

    /* rows on screen: the same scale as the width */
    shown_acc += VIEW_W;
    if (shown_acc < WEFAX_WIDTH)
        return;
    shown_acc -= WEFAX_WIDTH;
    memcpy(shown[shown_head], row, WEFAX_WIDTH);
    shown_head = (shown_head + 1) % VIEW_H;
    if (shown_n < VIEW_H)
        shown_n++;

    /* with the tiles over it the history goes on, the drawing waits for them to close */
    if (!wefax_bulletin_is_open())
        draw_live();        /* all rows: Tilt depends on the distance from the newest */
}

/* ---- view ---- */

/* Small: a low panel over the buttons, the waterfall above; Big/Full: the common app look */
static void apply_view(void)
{
    if (!dialog.obj || !canvas || !viewport)
        return;

    if (view == VIEW_SMALL) {
        lv_obj_set_size(dialog.obj, TEXT_PANEL_W, 182);
        lv_obj_align(dialog.obj, LV_ALIGN_CENTER, 0, 84);
        lv_obj_set_style_bg_img_src(dialog.obj, NULL, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dialog.obj, LV_OPA_TRANSP, LV_PART_MAIN);

        /* the canvas keeps its size, bottom aligned: only the older rows are cut */
        lv_obj_set_size(viewport, VIEW_W, 152);
        lv_obj_align(viewport, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(viewport, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(viewport, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(viewport, 0, LV_PART_MAIN);

        lv_obj_add_flag(frame.head_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(frame.head_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(frame.status, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(canvas, VIEW_W, VIEW_H);
        lv_obj_align(canvas, LV_ALIGN_BOTTOM_MID, 0, 0);

        if (panel) {
            lv_obj_set_size(panel, TEXT_PANEL_W, 182);
            lv_obj_center(panel);
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_background(panel);
        }
    } else {
        lv_obj_set_size(dialog.obj, TEXT_PANEL_W, DIALOG_H);
        lv_obj_align(dialog.obj, LV_ALIGN_TOP_MID, 0, DIALOG_Y);
        lv_obj_remove_local_style_prop(dialog.obj, LV_STYLE_BG_IMG_SRC, LV_PART_MAIN);
        lv_obj_remove_local_style_prop(dialog.obj, LV_STYLE_BG_OPA, LV_PART_MAIN);

        lv_obj_clear_flag(frame.head_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(frame.head_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(frame.status, LV_OBJ_FLAG_HIDDEN);

        app_frame_body(viewport);
        lv_obj_clear_flag(viewport, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(canvas, VIEW_W, VIEW_H);
        lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);

        if (panel)
            lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    }
    idle_update();
}

/* The status line: what the buttons and the MFK do where the app is */
#define HINT_RX     "Page 2: Freq., View   Page 3: Align, Tilt (MFK)   Page 4: Night"

static void hint(const char *text)
{
    if (frame.status)
        lv_label_set_text(frame.status, text);
}

static void focus_canvas(void)
{
    lv_group_add_obj(keyboard_group, canvas);
    lv_group_focus_obj(canvas);
    lv_group_set_editing(keyboard_group, true);
}

/* ---- dialog ---- */

static void app_open(lv_obj_t *parent)
{
    app_parent     = parent;
    view           = VIEW_BIG;          /* the common app look; Small shows the waterfall */
    btn_view.label = "View\nBig";       /* the label names the view on screen */
    align_on       = false;
    gen_seen       = 0;
    fax_on         = false;
    freqs_build();                      /* the header names a channel from the start */

    decoder = wefax_decoder_create();
    if (!decoder) {
        fprintf(stderr, "WEFAX: can't create the decoder\n");
    } else {
        wefax_decoder_set_row_callback(decoder, row_cb, NULL);
        wefax_decoder_set_mode(decoder, WEFAX_MODE_AUTO);      /* open in Auto, as the header and the RX button say */
    }

    dialog.obj = dialog_init(parent);
    lv_obj_set_size(dialog.obj, TEXT_PANEL_W, DIALOG_H);
    lv_obj_align(dialog.obj, LV_ALIGN_TOP_MID, 0, DIALOG_Y);

    /* no background image of dialog_init: its own white edges would show around ours */
    lv_obj_set_style_bg_img_src(dialog.obj, NULL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dialog.obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog.obj, 0, LV_PART_MAIN);

    /* transparent clip of the canvas (in Small it shows only the bottom of it) */
    viewport = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(viewport);
    lv_obj_set_size(viewport, TEXT_PANEL_W, DIALOG_H);
    lv_obj_center(viewport);
    lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(viewport, LV_OBJ_FLAG_CLICKABLE);

    canvas = lv_canvas_create(viewport);
    lv_canvas_set_buffer(canvas, view_buf, VIEW_W, VIEW_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(canvas, VIEW_W, VIEW_H);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);
    idle_create();                      /* over the canvas */

    /* the panel behind Small: panel colours flat (the panel image has its own outline), one border */
    panel = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(panel);
    lv_obj_add_style(panel, &panel_style, 0);
    lv_obj_set_style_bg_img_src(panel, NULL, LV_PART_MAIN);
    lv_obj_set_size(panel, TEXT_PANEL_W, DIALOG_H);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, TC(frame), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, TC(dialog_border), LV_PART_MAIN);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 8, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    app_frame_create(&frame, dialog.obj);
    hint(HINT_RX);

    /* the canvas takes the keys: the main screen's key handler is off while a dialog is open */
    lv_group_add_obj(keyboard_group, canvas);
    lv_obj_add_event_cb(canvas, app_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(canvas);
    lv_group_set_editing(keyboard_group, true);

    clear_fax();
    apply_view();
    view_timer = lv_timer_create(view_timer_cb, VIEW_MS, NULL);

    plan_on    = false;
    plan_dial  = 0;
    join_probe = false;
    plan_btn[0] = '\0';
    wefax_plan_load();
    plan_show(-1);
    plan_timer = lv_timer_create(plan_timer_cb, 1000, NULL);
    if (param_i_get(cfg_wefax_plan))
        plan_start();                   /* off-grid: open the app and the charts come */
}

static void app_close(void)
{
    /* closing the app stops a TX: wait for the worker to unkey the radio */
    if (tx_thread_on) {
        tx_abort = true;
        pthread_join(tx_thread, NULL);
        tx_thread_on = false;
    }

    freq_list_close();
    if (view_timer) {
        lv_timer_del(view_timer);
        view_timer = NULL;
    }
    if (plan_timer) {
        lv_timer_del(plan_timer);
        plan_timer = NULL;
    }
    plan_on       = false;
    plan_dial     = 0;
    join_probe    = false;
    btn_plan.mark = false;
    plan_btn[0]   = '\0';
    plan_next[0]  = '\0';

    align_on = tilt_on = false;
    wefax_bulletin_close();
    wefax_viewer_close();       /* after the tiles: its close gives the keys to nothing then */
    canvas          = NULL;
    viewport        = NULL;
    panel           = NULL;
    idle            = NULL;     /* deleted with the dialog */
    plan_next_i     = -1;
    frame.head_left = NULL;     /* deleted with the dialog */

    if (decoder) {
        wefax_decoder_destroy(decoder);
        decoder = NULL;
    }

    pthread_mutex_lock(&row_lock);
    row_full = false;
    pthread_mutex_unlock(&row_lock);

    shown_acc = 0;
    store_n = store_head = 0;
    free(store);
    store = NULL;
    rstat_update();     /* the decoder gone: the web page sees the app closed */
}

static void app_key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    /* VOL as on the main screen, whatever the MFK does: turned it changes what it is on (volume, RF gain, the
     * filter edges...), a press switches to choosing that and back (#226) */
    switch (key) {
        case KEY_VOL_LEFT_EDIT:
            vol_update(-1);
            return;
        case KEY_VOL_RIGHT_EDIT:
            vol_update(1);
            return;
        case KEY_VOL_LEFT_SELECT:
            vol_change_ctrl(-1);
            return;
        case KEY_VOL_RIGHT_SELECT:
            vol_change_ctrl(1);
            return;
        case KEY_VOL_PRESS:
            main_screen_vol_press();
            return;
        default:
            break;
    }

    if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT)
        return;

    bool left = key == LV_KEY_LEFT;

    if (align_on) {
        mfk_update(left ? -8 : 8);
        buttons_refresh(&btn_align);
        draw_live();
    } else if (tilt_on) {
        mfk_update(left ? -1 : 1);
        buttons_refresh(&btn_tilt);
        draw_live();
    }
}

/* ---- buttons ---- */

/* The tiles away, the keys back to the picture, drawn again */
static void bulletin_hide(void)
{
    if (!wefax_bulletin_is_open())
        return;
    wefax_bulletin_close();
    if (canvas)
        focus_canvas();
    draw_live();
    idle_update();
}

static void page_1_cb(button_data_t *data)
{
    (void)data;
    wefax_viewer_close();
    bulletin_hide();
    align_on = tilt_on = false;
    buttons_load_page(&page_1);
}

static void page_2_cb(button_data_t *data)
{
    (void)data;
    bulletin_hide();
    align_on = tilt_on = false;
    buttons_load_page(&page_2);
}

static void page_3_cb(button_data_t *data)
{
    (void)data;
    bulletin_hide();
    align_on = tilt_on = false;
    buttons_load_page(&page_3);
}

static void page_4_cb(button_data_t *data)
{
    (void)data;
    bulletin_hide();
    align_on = tilt_on = false;
    buttons_load_page(&page_4);
}

/* The chart Auto Plan is taking, for its tile */
static void bulletin_live(wefax_bulletin_live_t *out)
{
    const wefax_slot_t *s = fax_on && store_n ? wefax_plan_slot(fax_slot) : NULL;

    if (!s)
        return;

    long p = s->dur ? (long)(time(NULL) - fax_t0) * 1000 / s->dur : 0;

    out->product  = s->product;
    out->permille = p < 0 ? 0 : p > 999 ? 999 : (int)p;
    out->rows     = store_n;
    out->row      = store_row;
}

static void set_disabled(button_data_t *b, bool off)
{
    if (b->disabled != off)
        buttons_disabled(b, off);
}

/* The buttons of where the tiles are: the Bulletin's or All Charts', a chart large; Delete asked or not; Full
 * Screen, Fix and Delete only for a chart there */
static void tiles_buttons(void)
{
    buttons_page_t *want;
    bool            has = wefax_bulletin_has_chart();

    if (!wefax_bulletin_is_open() || wefax_viewer_is_open())
        return;
    want = wefax_bulletin_one() ? &page_chart : wefax_bulletin_mode() == WEFAX_TILES_ALL ? &page_all : &page_bul;
    btn_chart_page.label = wefax_bulletin_mode() == WEFAX_TILES_ALL ? "(All Charts)" : "(Bulletin)";
    set_disabled(&btn_full, !has);
    set_disabled(&btn_fix, !has);
    set_disabled(&btn_delete, !has);
    if (buttons_get_cur_page() != want)
        buttons_load_page(want);
    buttons_mark(&btn_delete, wefax_bulletin_delete_armed());
    if (btn_delete.disp_btn)
        buttons_refresh(&btn_delete);
}

/* The viewer gone: the tiles' buttons and keys back, a chart saved there shown again */
static void viewer_closed(void)
{
    if (!wefax_bulletin_is_open())
        return;
    wefax_bulletin_focus();
    tiles_buttons();
}

static void viewer_back_cb(button_data_t *data)
{
    (void)data;
    wefax_viewer_close();
}

static void tiles_view(const char *name, const char *title, bool fix)
{
    if (wefax_viewer_open(SAVE_DIR, name, title, fix, viewer_closed))
        buttons_load_page(fix ? &page_fix : &page_viewer);
    else
        channel_overlay_show("Can't read this chart");
}

/* ESC on the tiles: back to the fax, as F5 */
static void tiles_back_to_fax(void)
{
    page_1_cb(NULL);
}

/* The tiles over the app in the Big view */
static void tiles_open(wefax_tiles_mode_t mode)
{
    align_on = tilt_on = false;
    if (view == VIEW_SMALL) {
        view           = VIEW_BIG;
        btn_view.label = "View\nBig";
        apply_view();
    }
    if (canvas)
        lv_group_remove_obj(canvas);
    wefax_preview_sweep(SAVE_DIR);      /* the previews of charts deleted elsewhere (the web Gallery) */

    wefax_bulletin_cbs_t cbs = {.live       = bulletin_live,
                                .other_keys = app_key_cb,
                                .changed    = tiles_buttons,
                                .view_file  = tiles_view,
                                .back       = tiles_back_to_fax};

    wefax_bulletin_open(dialog.obj, mode, &cbs);
    tiles_buttons();
}

/* Bulletin: each kind of chart as a tile (wefax_bulletin.h) */
static void bulletin_cb(button_data_t *data)
{
    (void)data;
    bulletin_hide();
    tiles_open(WEFAX_TILES_BULLETIN);
}

/* All Charts: every chart saved, newest first */
static void all_charts_cb(button_data_t *data)
{
    (void)data;
    bulletin_hide();
    tiles_open(WEFAX_TILES_ALL);
}

static void tiles_open_cb(button_data_t *data)
{
    (void)data;
    wefax_bulletin_enter();
}

static void tiles_newest_cb(button_data_t *data)
{
    (void)data;
    wefax_bulletin_newest();
}

static const char *tiles_delete_label(void)
{
    return wefax_bulletin_delete_armed() ? "Delete?\nPress again" : "Delete";
}

static void tiles_delete_cb(button_data_t *data)
{
    (void)data;
    wefax_bulletin_delete();
}

/* F5: a chart back to the tiles, the tiles back to the fax */
static void tiles_back_cb(button_data_t *data)
{
    (void)data;
    if (!wefax_bulletin_back())
        page_1_cb(NULL);
}

static void chart_full_cb(button_data_t *data)
{
    (void)data;
    wefax_bulletin_enter();
}

/* Fix: from the large view over the whole screen straight into Fix; from the full screen, Fix there */
static void chart_fix_cb(button_data_t *data)
{
    (void)data;
    if (wefax_viewer_is_open()) {
        wefax_viewer_fix();
        buttons_load_page(&page_fix);
        return;
    }
    wefax_bulletin_fix();
}

static void fix_auto_cb(button_data_t *data)
{
    (void)data;
    wefax_viewer_auto_tilt();
}

static void fix_undo_cb(button_data_t *data)
{
    (void)data;
    wefax_viewer_undo();
}

static void fix_cancel_cb(button_data_t *data)
{
    (void)data;
    wefax_viewer_cancel();
}

static void fix_save_cb(button_data_t *data)
{
    (void)data;
    wefax_viewer_save();
}

static const char *night_label(void)
{
    return wefax_night_label();
}

/* Night Auto -> On -> Off: the live picture drawn again at once */
static void night_cb(button_data_t *data)
{
    (void)data;
    wefax_night_next();
    buttons_refresh(&btn_night);
    draw_live();
}

static void close_cb(button_data_t *data)
{
    (void)data;
    dialog_destruct();
}

static void freq_cb(button_data_t *data)
{
    (void)data;
    if (freq_list_is_open()) {
        freq_list_close();
    } else {
        freqs_build();      /* the list keeps pointers into freqs while it is open */
        freq_list_open(app_parent, "WEFAX - stations", freqs, freqs_n, WEFAX_CENTER_HZ, NULL);
    }
}

/* Cont: rows at once (tuned in during a fax) */
static void rx_cont(void)
{
    if (!decoder)
        return;
    channel_overlay_show("Receiving Fax");
    wefax_decoder_set_mode(decoder, WEFAX_MODE_CONT);
    clear_fax();
    printf("WEFAX: Cont\n");
}

/* Auto: waits for the start tone */
static void rx_auto(void)
{
    if (!decoder)
        return;
    channel_overlay_show("Waiting for Fax");
    wefax_decoder_set_mode(decoder, WEFAX_MODE_AUTO);
    fax_on = false;
    clear_fax();
    printf("WEFAX: Auto, waiting for the start tone\n");
}

static const char *rx_mode_label(void)
{
    return decoder && wefax_decoder_get_mode(decoder) == WEFAX_MODE_CONT ? "RX\nCont" : "RX\nAuto";
}

static void rx_mode_cb(button_data_t *data)
{
    (void)data;
    if (auto_mode())
        rx_cont();
    else
        rx_auto();
    buttons_refresh(&btn_rx_mode);
}

static void clear_cb(button_data_t *data)
{
    (void)data;
    if (!decoder)
        return;
    rx_restart();
    clear_fax();
}

static const char *align_label(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), "Align\n%+i", (int)param_i_get(cfg_wefax_align));
    return buf;
}

static void align_cb(button_data_t *data)
{
    (void)data;
    align_on = true;
    tilt_on  = false;
    mfk_set_ctrl(CTRL_WEFAX_ALIGN);
}

static const char *tilt_label(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), "Tilt\n%+i", (int)param_i_get(cfg_wefax_tilt));
    return buf;
}

static void tilt_cb(button_data_t *data)
{
    (void)data;
    align_on = false;
    tilt_on  = true;
    mfk_set_ctrl(CTRL_WEFAX_TILT);
}

static void view_cb(button_data_t *data)
{
    (void)data;

    static const char *labels[] = {"View\nSmall", "View\nBig", "View\nFull Size"};

    view           = (view + 1) % 3;
    btn_view.label = labels[view];
    apply_view();
    buttons_load_page(&page_2);
    draw_live();
}

/* Save now; the receiver starts over once the copy is taken */
static void save_cb(button_data_t *data)
{
    (void)data;
    if (decoder && save_fax(true)) {
        wefax_decoder_reset(decoder);
        gen_seen = 0;
        fax_on   = false;
        clear_fax();
    }
}

bool wefax_window_remote(wefax_rc_t what)
{
    if (!dialog_type_is_run(dialog_wefax) || !decoder)
        return false;
    switch (what) {
        case WEFAX_RC_AUTO:
            if (!auto_mode())
                rx_auto();
            break;
        case WEFAX_RC_CONT:
            if (auto_mode())
                rx_cont();
            break;
        case WEFAX_RC_SAVE:   save_cb(NULL); break;
        case WEFAX_RC_CLEAR:  clear_cb(NULL); break;
        case WEFAX_RC_PLAN:   plan_start(); break;
        case WEFAX_RC_UNPLAN: plan_off(); break;
    }
    buttons_refresh(&btn_rx_mode);
    return true;
}
