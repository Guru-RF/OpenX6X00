// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - NavTex app: NAVTEX / SITOR-B text, and DSC (ITU-R M.493,
// receive only) on the same demodulator
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY

#include "navtex_window.h"
#include "board.h"
#include "navtex/sitorb.h"
#include "navtex/dsc.h"
#include "ft8/decode_list.h"
#include "flags.h"
#include "cJSON.h"
#include "app_window.h"
#include "gui_styles.h"
#include "theme.h"
#include "button_bar.h"
#include "key_input.h"
#include "radio.h"          /* radio_change_vol */
#include "freq_list.h"
#include "freq_strip.h"
#include "app_frame.h"
#include "cfg/cfg_api.h"
#include "dsp.h"
#include "gui_events.h"
#include "msg_box.h"

#include <pthread.h>        /* rx_lock */
#include <stdio.h>          /* snprintf, the Save file */
#include <stdlib.h>         /* free */
#include <string.h>         /* memcpy, strcmp */
#include <sys/stat.h>
#include <time.h>

#define NAVTEX_SAVE_DIR BOARD_DATA "/navtex"   /* navtex-YYYYMMDD-HHMMSS.txt, on the web Files page (idea from 1KO125) */

#define TEXT_SIZE   65536   /* the text on screen; whole lines go from the top when it is full */
#define RX_CHARS    512     /* characters waiting for the UI timer */
#define UI_TICK_MS  50
#define SCROLL_PX   36      /* one MFK step */

#define DSC_ROWS_MAX 100
#define DSC_PENDING_MAX 8
#define DSC_REPEAT_S 120      /* a distress alert is repeated: the same call again within this is skipped */

static navtex_decoder_t *decoder;

/* What the audio thread hands over (under rx_lock): characters, and DSC calls */
static pthread_mutex_t rx_lock = PTHREAD_MUTEX_INITIALIZER;
static char            rx_chars[RX_CHARS];
static size_t          rx_nchars;

/* DSC: the mode, and the calls from the audio thread (under rx_lock) to the UI */
static volatile bool dsc_mode = false;
static volatile bool dsc_reset = true;
static dsc_rx_t dsc_rx;
static dsc_call_t dsc_pending[DSC_PENDING_MAX];
static int dsc_npending = 0;
static lv_obj_t *dsc_list = NULL;
static lv_font_t dsc_font;    /* sony_18 with Montserrat behind it for the degree sign and the arrow */

/* NAVTEX text, a plain C string for the label */
static char   screen_text[TEXT_SIZE];
static size_t screen_len;
static bool   follow;         /* the view stays on the newest line */

static lv_obj_t   *scroll;
static lv_obj_t   *label;
static lv_timer_t *ui_timer;
static app_frame_t frame;

static void app_open(lv_obj_t *parent);
static void app_close(void);
static void audio_in(unsigned int n, float *samples);
static void body_key_cb(lv_event_t *e);
static void page_cb(button_data_t *data);
static void freq_cb(button_data_t *data);
static void mode_cb(button_data_t *data);
static void clear_btn_cb(button_data_t *btn);
static void close_btn_cb(button_data_t *btn);
static void save_cb(button_data_t *data);
static const char *mode_label(void);
static const char *page1_label(void);
static const char *page2_label(void);

static button_data_t btn_page   = {.type = BTN_TEXT_FN, .label_fn = page1_label, .press = page_cb};
static button_data_t btn_freq   = {.type = BTN_TEXT, .label = "Freq.", .press = freq_cb};
static button_data_t btn_mode   = {.type = BTN_TEXT_FN, .label_fn = mode_label, .press = mode_cb};
static button_data_t btn_clear  = {.type = BTN_TEXT, .label = "Clear", .press = clear_btn_cb};
static button_data_t btn_close  = {.type = BTN_TEXT, .label = "Close", .press = close_btn_cb};
static button_data_t btn_page_2 = {.type = BTN_TEXT_FN, .label_fn = page2_label, .press = page_cb};
static button_data_t btn_save   = {.type = BTN_TEXT, .label = "Save", .press = save_cb};

static buttons_page_t page_1 = {.items = {&btn_page, &btn_freq, &btn_mode, &btn_clear, &btn_close}};
static buttons_page_t page_2 = {.items = {&btn_page_2, &btn_save, NULL, NULL, NULL}};

static dialog_t dialog = {.construct_cb = app_open, .destruct_cb = app_close, .audio_cb = audio_in,
                          .btn_page = &page_1, .restore_radio = true};

dialog_t *dialog_navtex = &dialog;     /* the APP menu opens it */

/* Official NAVTEX frequencies, plus the HF MSI ones (same SITOR-B mode), then the
 * MF/HF DSC distress and safety channels (DSC mode, NAVTEX_N on).
 * The decoder is centred on 1000 Hz: B (0) is the higher tone, Y (1) the lower. */
#define NAVTEX_N 11
#define DSC_N    7
static const freq_list_item_t navtex_freqs[NAVTEX_N + DSC_N] = {
    { .group = "NAVTEX" },
    { .freq_hz = 518000,   .name = "NAVTEX",    .note = "International, English" },
    { .freq_hz = 490000,   .name = "NAVTEX",    .note = "National, local language" },
    { .freq_hz = 4209500,  .name = "NAVTEX HF", .note = "Tropical areas" },
    { .group = "HF MSI (NBDP)" },
    { .freq_hz = 8416500,  .name = "HF MSI" },
    { .freq_hz = 12579000, .name = "HF MSI" },
    { .freq_hz = 16806500, .name = "HF MSI" },
    { .freq_hz = 19680500, .name = "HF MSI" },
    { .freq_hz = 22376000, .name = "HF MSI" },
    { .freq_hz = 26100500, .name = "HF MSI" },
    { .group = "DSC distress / safety" },
    { .freq_hz = 2187500,  .name = "DSC", .note = "MF distress / safety" },
    { .freq_hz = 4207500,  .name = "DSC", .note = "HF distress / safety" },
    { .freq_hz = 6312000,  .name = "DSC", .note = "HF distress / safety" },
    { .freq_hz = 8414500,  .name = "DSC", .note = "HF distress / safety" },
    { .freq_hz = 12577000, .name = "DSC", .note = "HF distress / safety" },
    { .freq_hz = 16804500, .name = "DSC", .note = "HF distress / safety" },
};

#define DSC_DEFAULT (NAVTEX_N + 4)   /* 8414.5 kHz */

/* The mode's part of navtex_freqs */
static const freq_list_item_t *mode_freqs(int *n)
{
    *n = dsc_mode ? DSC_N : NAVTEX_N;
    return dsc_mode ? navtex_freqs + NAVTEX_N : navtex_freqs;
}

/* The info bar keeps the station's short name ("DSC", "HF MSI"): with the note it ran
 * under the frequency */
static void short_label(void)
{
    int n;
    int32_t dial = 0, vfo = cparam_i_get(cfg_fg_freq);
    const freq_list_item_t *list = mode_freqs(&n);
    const freq_list_item_t *it = freq_list_base(list, n, 1000, vfo, &dial);

    if (it && dial == vfo)
        freq_strip_set_label(it->name, dial);
}

static void tune(const freq_list_item_t *it)
{
    freq_list_tune(it, 1000);
    short_label();
}

bool dialog_navtex_is_active(void) { return dialog.run; }

/* The body to y; LV_COORD_MAX is the newest line */
static void body_scroll(lv_coord_t y)
{
    lv_obj_scroll_to_y(scroll, y, LV_ANIM_OFF);
}

/* New text at the end; a chunk longer than the whole buffer keeps only its tail */
static void text_add(const char *s, size_t n)
{
    size_t cut = 0;

    if (n > TEXT_SIZE - 1) {
        s += n - (TEXT_SIZE - 1);
        n = TEXT_SIZE - 1;
        screen_len = 0;
    }
    while (screen_len - cut + n > TEXT_SIZE - 1) {
        const char *nl = memchr(screen_text + cut, '\n', screen_len - cut);

        cut = nl ? (size_t)(nl - screen_text) + 1 : screen_len;
    }
    memmove(screen_text, screen_text + cut, screen_len - cut);
    screen_len -= cut;
    memcpy(screen_text + screen_len, s, n);
    screen_len += n;
    screen_text[screen_len] = '\0';
}

static void text_clear(void)
{
    pthread_mutex_lock(&rx_lock);
    rx_nchars = 0;
    pthread_mutex_unlock(&rx_lock);
    screen_len = 0;
    screen_text[0] = '\0';
    follow = true;
}

/* Audio thread: a decoded character, kept for the UI (NAVTEX mode only) */
static void char_cb(int c, void *user)
{
    (void)user;
    if (!dsc_mode && c != '\r') {
        pthread_mutex_lock(&rx_lock);
        if (rx_nchars < RX_CHARS - 1)
            rx_chars[rx_nchars++] = (char)c;
        pthread_mutex_unlock(&rx_lock);
    }
}

/* Audio thread: a DSC call to the UI */
static void dsc_call_cb(const dsc_call_t *call, void *userdata)
{
    (void)userdata;

    pthread_mutex_lock(&rx_lock);
    if (dsc_npending < DSC_PENDING_MAX)
        dsc_pending[dsc_npending++] = *call;
    pthread_mutex_unlock(&rx_lock);
}

/* Audio thread: the demodulated bits, in DSC mode; the higher tone is B (0) */
static void bit_cb(int mark, void *userdata)
{
    (void)userdata;

    if (!dsc_mode)
        return;
    if (dsc_reset) {
        dsc_rx_init(&dsc_rx, dsc_call_cb, NULL);
        dsc_reset = false;
    }
    dsc_rx_bit(&dsc_rx, !mark);
}

/* Station and frequency on the left; the right side always says RX */
static void header_update(void)
{
    char         l[80];
    int32_t      f = cparam_i_get(cfg_fg_freq) + 1000;     /* USB, decoder centred on 1000 Hz */
    const char  *name = dsc_mode ? "DSC" : "NAVTEX", *note = NULL;

    for (size_t i = 0; i < sizeof(navtex_freqs) / sizeof(navtex_freqs[0]); i++) {
        if (!navtex_freqs[i].group && navtex_freqs[i].freq_hz == f && (i >= NAVTEX_N) == dsc_mode) {
            name = navtex_freqs[i].name;
            note = navtex_freqs[i].note;
        }
    }
    if (dsc_mode && note)
        snprintf(l, sizeof(l), "%s   %.1f kHz   %s", name, f / 1000.0, note);
    else
        snprintf(l, sizeof(l), "%s   %.1f kHz", name, f / 1000.0);
    if (strcmp(l, lv_label_get_text(frame.head_left)))
        lv_label_set_text(frame.head_left, l);
}

/* DSC rows: the format's name for the remote */
static const char *fmt_name(int fmt)
{
    switch (fmt) {
        case 102: return "area";
        case 112: return "distress";
        case 114: return "group";
        case 116: return "all ships";
        case 120: return "individual";
        default: return "automatic";
    }
}

/* What the call is about, after the country; arrow before an addressed station */
static void dsc_detail(const dsc_call_t *c, const char *arrow, char *out, size_t size)
{
    const char *cat = dsc_cat_name(c), *nature = dsc_nature_name(c->nature);
    const char *tc = dsc_tc_name(c->tc1);
    char        target[48];

    if (c->fmt == 112) {
        snprintf(out, size, "%s%s", c->ecc_ok ? "" : "? ", nature[0] ? nature : "DISTRESS ALERT");
        return;
    }
    if (!strcmp(cat, "ack")) {
        snprintf(out, size, "%s%s %s  distress ack", c->ecc_ok ? "" : "? ", arrow, c->dmmsi);
        return;
    }
    if (c->fmt == 116)
        snprintf(target, sizeof(target), "ALL SHIPS");
    else if (c->fmt == 102)
        snprintf(target, sizeof(target), "AREA %s", c->to);
    else if (c->fmt == 114)
        snprintf(target, sizeof(target), "%s group %s", arrow, c->to);
    else
        snprintf(target, sizeof(target), "%s %s", arrow, c->to);
    if (c->dmmsi[0])   /* distress relay */
        snprintf(out, size, "%s%s  RELAY %s %s", c->ecc_ok ? "" : "? ", target, c->dmmsi, nature);
    else
        snprintf(out, size, "%s%s  %s%s%s", c->ecc_ok ? "" : "? ", target, tc, tc[0] && c->freq[0] ? " " : "",
                 c->freq);
}

/* Distress alerts and relays: position and time on a second line */
static void dsc_where(const dsc_call_t *c, char *out, size_t size)
{
    out[0] = 0;
    if (!c->dmmsi[0] || !strcmp(dsc_cat_name(c), "ack"))
        return;
    snprintf(out, size, "%s%s%s%s", c->pos[0] ? c->pos : "position unknown", c->utc[0] ? "  at " : "", c->utc,
             c->utc[0] ? " UTC" : "");
}

/* "Italy", "Coast, Spain", "" */
static const char *dsc_from_country(const char *mmsi, char *buf, size_t size, int *flag)
{
    const char *prefix = NULL, *name = dsc_country(mmsi, &prefix);

    *flag = -1;
    if (!name)
        return "";
    for (size_t i = 0; i < sizeof(flag_of) / sizeof(flag_of[0]); i++)
        if (!strcmp(flag_of[i].prefix, prefix)) {
            *flag = flag_of[i].flag;
            break;
        }
    if (mmsi[0] == '0' && mmsi[1] == '0') {
        snprintf(buf, size, "Coast, %s", name);
        return buf;
    }
    return name;
}

static void row_delete_cb(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_target(e)));
}

static lv_obj_t *row_label(lv_obj_t *row, lv_coord_t x, lv_coord_t y, lv_coord_t w, const char *text, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(row);

    lv_obj_set_style_text_font(l, &dsc_font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, text);
    return l;
}

/* A row at the top of the list: time, category, MMSI, flag and country, detail */
static void dsc_add_row(const dsc_call_t *c, const char *hms, int32_t freq)
{
    static dsc_call_t last;
    static time_t     last_t;
    time_t            now = time(NULL);
    char              detail[96], where[64], cty_buf[48], line[256];
    const char       *cat = dsc_cat_name(c), *pill;
    int               flag;
    bool              distress = !strcmp(cat, "distress");
    lv_color_t        pill_bg, pill_fg = TC(text);

    if (!memcmp(&last, c, sizeof(last)) && now - last_t < DSC_REPEAT_S)
        return;
    last   = *c;
    last_t = now;

    const char *country = dsc_from_country(c->from, cty_buf, sizeof(cty_buf), &flag);
    dsc_detail(c, LV_SYMBOL_RIGHT, detail, sizeof(detail));
    dsc_where(c, where, sizeof(where));

    /* The text Save writes */
    {
        char plain[96];
        dsc_detail(c, "->", plain, sizeof(plain));
        snprintf(line, sizeof(line), "%s %-8s %s %s %s%s%s\n", hms, cat, c->from, country, plain, where[0] ? "  " : "",
                 where);
    }
    if (!dsc_list)
        return;

    if (distress) {
        pill = "DISTRESS"; pill_bg = TC(tx_bg);
    } else if (!strcmp(cat, "ack")) {
        pill = "ACK"; pill_bg = TC(toggle_on);
    } else if (!strcmp(cat, "urgency")) {
        pill = "URGENCY"; pill_bg = TC(warn); pill_fg = TC(panel);
    } else if (!strcmp(cat, "safety")) {
        pill = "SAFETY"; pill_bg = TC(accent); pill_fg = TC(panel);
    } else {
        pill = "ROUTINE"; pill_bg = TC(off);
    }

    lv_obj_t *row = lv_obj_create(dsc_list);
    lv_obj_remove_style_all(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, lv_pct(100), where[0] ? 54 : 30);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, TC(row), 0);
    lv_obj_set_user_data(row, strdup(line));
    lv_obj_add_event_cb(row, row_delete_cb, LV_EVENT_DELETE, NULL);
    if (distress) {
        lv_obj_set_style_bg_color(row, TC(err_bg), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_t *edge = lv_obj_create(row);
        lv_obj_remove_style_all(edge);
        lv_obj_set_size(edge, 3, lv_pct(100));
        lv_obj_set_style_bg_color(edge, TC(err_strong), 0);
        lv_obj_set_style_bg_opa(edge, LV_OPA_COVER, 0);
    }

    row_label(row, 8, 4, 76, hms, c->ecc_ok ? TC(text_dim) : TC(warn));

    lv_obj_t *p = row_label(row, 90, 4, 88, pill, pill_fg);
    lv_obj_set_style_text_font(p, &sony_16, 0);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(p, pill_bg, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 4, 0);
    lv_obj_set_style_pad_ver(p, 1, 0);

    row_label(row, 190, 4, 104, c->from, TC(text));
    const lv_img_dsc_t *img = table_view_flag(flag);
    if (img) {
        lv_obj_t *i = lv_img_create(row);
        lv_img_set_src(i, img);
        lv_obj_set_pos(i, 300, 9);
    }
    row_label(row, 322, 4, 150, country, TC(text_dim));
    row_label(row, 480, 4, 270, detail, TC(text));
    if (where[0])
        row_label(row, 190, 28, 560, where, TC(text));

    lv_obj_move_to_index(row, 0);   /* the newest on top */
    if (lv_obj_get_child_cnt(dsc_list) > DSC_ROWS_MAX)
        lv_obj_del(lv_obj_get_child(dsc_list, -1));
}

/* Every UI_TICK_MS while the app is open: the header, then what the audio thread decoded */
static void ui_timer_cb(lv_timer_t *t)
{
    char       chars[RX_CHARS];
    dsc_call_t calls[DSC_PENDING_MAX];
    size_t     nchars;
    int        ncalls;

    (void)t;
    header_update();

    pthread_mutex_lock(&rx_lock);
    nchars = rx_nchars;
    memcpy(chars, rx_chars, nchars);
    rx_nchars = 0;
    ncalls = dsc_npending;
    memcpy(calls, dsc_pending, ncalls * sizeof(calls[0]));
    dsc_npending = 0;
    pthread_mutex_unlock(&rx_lock);

    if (ncalls) {
        time_t now = time(NULL);
        char   hms[16];
        strftime(hms, sizeof(hms), "%H:%M:%S", gmtime(&now));
        for (int i = 0; i < ncalls; i++)
            dsc_add_row(&calls[i], hms, cparam_i_get(cfg_fg_freq) + 1000);
    }
    if (nchars) {
        text_add(chars, nchars);
        lv_label_set_text(label, screen_text);
        if (follow)
            body_scroll(LV_COORD_MAX);
    }
}

/* The mode's content, status line and buttons */
static void mode_show(void)
{
    if (!scroll)
        return;
    if (dsc_mode) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(dsc_list, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(frame.status,
                          "DSC 100 Bd / 170 Hz   MFK: scroll   Freq.: channels");
    } else {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dsc_list, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(frame.status, "SITOR-B 100 Bd / 170 Hz   MFK: scroll   Freq.: stations");
    }
    body_scroll(dsc_mode ? 0 : LV_COORD_MAX);
    if (btn_mode.disp_btn) buttons_refresh(&btn_mode);
    if (btn_page.disp_btn) buttons_refresh(&btn_page);
    if (btn_page_2.disp_btn) buttons_refresh(&btn_page_2);
    header_update();
}

static void app_open(lv_obj_t *parent)
{
    pthread_mutex_lock(&rx_lock);
    dsc_npending = 0;
    pthread_mutex_unlock(&rx_lock);
    text_clear();

    decoder = navtex_decoder_create(0);
    if (decoder) {
        navtex_decoder_set_char_callback(decoder, char_cb, NULL);
        navtex_decoder_set_bit_callback(decoder, bit_cb, NULL);
    } else {
        fprintf(stderr, "NAVTEX: unable to create decoder\n");
    }
    dsc_reset = true;

    /* Full screen in the common app look; the main waterfall is hidden */
    dialog.obj = dialog_init(parent);
    dsp_set_waterfall_enabled(false);
    dsp_set_spectrum_enabled(false);
    app_frame_create(&frame, dialog.obj);
    lv_label_set_text(frame.head_right, "RX");

    lv_obj_t *body = lv_obj_create(dialog.obj);

    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    scroll = body;

    /* Width in percent: the body has had no layout pass yet, its content width is still 0 */
    lv_obj_t *txt = lv_label_create(body);

    lv_obj_set_width(txt, lv_pct(100));
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(txt, &sony_22, 0);
    lv_obj_set_style_text_color(txt, TC(text), 0);
    lv_label_set_text(txt, "NAVTEX ready - 100 baud / 170 Hz shift\n");
    label = txt;

    /* DSC calls, the newest on top */
    dsc_font = sony_18;
    dsc_font.fallback = &lv_font_montserrat_18;
    dsc_list = lv_obj_create(body);
    lv_obj_remove_style_all(dsc_list);
    lv_obj_set_size(dsc_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dsc_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(dsc_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* The body takes the MFK (scrolling) and the volume knob, see body_key_cb */
    lv_group_add_obj(keyboard_group, body);
    lv_obj_add_event_cb(body, body_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(body);
    lv_group_set_editing(lv_obj_get_group(body), true);

    ui_timer = lv_timer_create(ui_timer_cb, UI_TICK_MS, NULL);
    mode_show();
}

static void app_close(void)
{
    navtex_decoder_t *d = decoder;

    freq_list_close();
    lv_timer_del(ui_timer); ui_timer = NULL;
    decoder = NULL;     /* audio_in sees no decoder from here */
    if (d)
        navtex_decoder_destroy(d);
    pthread_mutex_lock(&rx_lock);
    rx_nchars = 0;
    pthread_mutex_unlock(&rx_lock);

    label = scroll = dsc_list = NULL;
    dsp_set_waterfall_enabled(true);
    dsp_set_spectrum_enabled(true);
}

static void audio_in(unsigned int n, float *samples)
{
    navtex_decoder_t *d = decoder;

    if (d)
        navtex_decoder_process(d, samples, n);
}

/* MFK left goes back through the text and stops following it; right goes forward, and a
 * step that no longer moves means the bottom: follow again. With an app open the main
 * screen does not see the volume knob, so it is turned here. */
static void body_key_cb(lv_event_t *e)
{
    uint32_t   key = *(uint32_t *)lv_event_get_param(e);
    bool       vol_down = key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT;
    bool       vol_up = key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT;
    lv_coord_t y = lv_obj_get_scroll_y(scroll);   /* before the step */

    if (vol_down || vol_up) {
        radio_change_vol(vol_up ? 1 : -1);
    } else if (key == LV_KEY_LEFT) {
        follow = false;
        body_scroll(y - SCROLL_PX);
    } else if (key == LV_KEY_RIGHT) {
        body_scroll(y + SCROLL_PX);
        if (lv_obj_get_scroll_y(scroll) == y) {
            follow = true;
            body_scroll(LV_COORD_MAX);
        }
    }
}

static void close_btn_cb(button_data_t *btn) { (void)btn; dialog_destruct(); }

/* NAVTEX: the text and what is still waiting; DSC: the rows */
static void clear_btn_cb(button_data_t *btn)
{
    (void)btn;
    if (dsc_mode) {
        lv_obj_clean(dsc_list);
        return;
    }
    text_clear();
    lv_label_set_text(label, screen_text);   /* empty now */
    body_scroll(0);
}

/* The text on screen to a file, then cleared; DSC: the calls, oldest first */
static void save_cb(button_data_t *data)
{
    char   name[64];
    time_t now = time(NULL);
    FILE  *f;
    bool   ok = true;
    int    rows = dsc_list ? (int)lv_obj_get_child_cnt(dsc_list) : 0;

    if (dsc_mode ? !rows : !screen_len) {
        msg_update_text_fmt("Nothing to save");
        return;
    }
    mkdir(NAVTEX_SAVE_DIR, 0755);
    strftime(name, sizeof(name), dsc_mode ? NAVTEX_SAVE_DIR "/dsc-%Y%m%d-%H%M%S.txt"
                                          : NAVTEX_SAVE_DIR "/navtex-%Y%m%d-%H%M%S.txt", gmtime(&now));
    f = fopen(name, "w");
    if (f && dsc_mode) {
        for (int i = rows - 1; i >= 0 && ok; i--)
            ok = fputs(lv_obj_get_user_data(lv_obj_get_child(dsc_list, i)), f) >= 0;
    } else if (f) {
        ok = fwrite(screen_text, 1, screen_len, f) == screen_len;
    }
    if (!f || !ok) {
        if (f) fclose(f);
        msg_update_text_fmt("NAVTEX: save failed");
        return;
    }
    fclose(f);
    clear_btn_cb(data);
    msg_update_text_fmt("Saved %s", name + sizeof(NAVTEX_SAVE_DIR));
}

/* The mode's list of stations / channels */
static void freq_cb(button_data_t *data)
{
    (void)data;
    int n;
    const freq_list_item_t *list = mode_freqs(&n);

    if (freq_list_is_open())
        freq_list_close();
    else if (scroll)
        freq_list_open(lv_obj_get_parent(scroll), dsc_mode ? "DSC - channels" : "NAVTEX - frequencies", list, n,
                       1000, short_label);
}

/* NAVTEX <-> DSC; the tuning follows when it was on the other mode's channels */
static void mode_set(bool dsc)
{
    int n;
    const freq_list_item_t *list;
    int32_t dial = 0;

    if (dsc == dsc_mode)
        return;
    freq_list_close();
    list = mode_freqs(&n);
    freq_list_base(list, n, 1000, cparam_i_get(cfg_fg_freq), &dial);
    bool on_channel = dial == cparam_i_get(cfg_fg_freq);

    dsc_mode = dsc;
    dsc_reset = true;
    if (on_channel)
        tune(dsc ? &navtex_freqs[DSC_DEFAULT] : &navtex_freqs[1]);
    mode_show();
}

static void mode_cb(button_data_t *data)
{
    (void)data;
    mode_set(!dsc_mode);
}

static void page_cb(button_data_t *data)
{
    buttons_load_page(data == &btn_page ? &page_2 : &page_1);
}

static const char *mode_label(void)
{
    return dsc_mode ? "Mode:\nDSC" : "Mode:\nNAVTEX";
}

static const char *page1_label(void)
{
    return dsc_mode ? "(DSC 1:2)" : "(NAVTEX 1:2)";
}

static const char *page2_label(void)
{
    return dsc_mode ? "(DSC 2:2)" : "(NAVTEX 2:2)";
}
