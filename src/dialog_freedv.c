// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - RADE app: FreeDV's RADE V1, receive only (src/freedv_rx.c does the work)

#include "dialog_freedv.h"
#include "freedv_rx.h"
#include "app_window.h"
#include "app_frame.h"
#include "button_bar.h"
#include "cfg/cfg_api.h"
#include "freq_list.h"
#include "freq_strip.h"
#include "gui_events.h"
#include "gui_styles.h"
#include "key_input.h"
#include "radio.h"          /* radio_change_vol */
#include "theme.h"
#include "dsp.h"

#include <stdio.h>
#include <string.h>

#define UI_TICK_MS 200
/* The RADE carriers sit at 750..2200 Hz above the dial frequency (rade_ofdm.c: 30 carriers of
 * 50 Hz around 1500 Hz): USB or LSB alike, the list's frequencies are the dial's */
#define AUDIO_HZ 0

static app_frame_t frame;
static lv_timer_t *ui_timer;
static bool        started;

static lv_obj_t *lbl_sync, *lbl_snr, *lbl_foff, *lbl_level, *lbl_audio, *lbl_loss, *lbl_cpu;

static void app_open(lv_obj_t *parent);
static void app_close(void);
static void audio_in(unsigned int n, float *samples);
static void body_key_cb(lv_event_t *e);
static void freq_cb(button_data_t *data);
static void close_btn_cb(button_data_t *btn);

static button_data_t btn_freq  = {.type = BTN_TEXT, .label = "Freq.", .press = freq_cb};
static button_data_t btn_close = {.type = BTN_TEXT, .label = "Close", .press = close_btn_cb};

static buttons_page_t page_1 = {.items = {&btn_freq, &btn_close, NULL, NULL, NULL}};

static dialog_t dialog = {.construct_cb = app_open, .destruct_cb = app_close, .audio_cb = audio_in,
                          .btn_page = &page_1, .restore_radio = true};

dialog_t *dialog_freedv = &dialog;

/* The FreeDV activity frequencies (dial, USB; 160 m to 40 m LSB), from the FreeDV Activity Day
 * 2026 announcement (digitalvoice group); they are for FreeDV in general, not for RADE alone */
static const freq_list_item_t freedv_freqs[] = {
    { .group = "160 m / 80 m" },
    { .freq_hz = 1997000,  .name = "FreeDV", .lsb = true },
    { .freq_hz = 3625000,  .name = "FreeDV", .lsb = true },
    { .freq_hz = 3643000,  .name = "FreeDV", .lsb = true },
    { .freq_hz = 3693000,  .name = "FreeDV", .lsb = true },
    { .freq_hz = 3697000,  .name = "FreeDV", .lsb = true },
    { .freq_hz = 3803000,  .name = "FreeDV", .lsb = true },
    { .group = "60 m (USB)" },
    { .freq_hz = 5366500,  .name = "FreeDV" },
    { .freq_hz = 5403500,  .name = "FreeDV" },
    { .group = "40 m" },
    { .freq_hz = 7177000,  .name = "FreeDV", .lsb = true },
    { .freq_hz = 7197000,  .name = "FreeDV", .lsb = true },
    { .group = "20 m and up" },
    { .freq_hz = 14236000, .name = "FreeDV" },
    { .freq_hz = 14240000, .name = "FreeDV" },
    { .freq_hz = 18118000, .name = "FreeDV" },
    { .freq_hz = 21313000, .name = "FreeDV" },
    { .freq_hz = 24933000, .name = "FreeDV" },
    { .freq_hz = 28330000, .name = "FreeDV" },
    { .freq_hz = 28720000, .name = "FreeDV" },
};
#define FREQS_N ((int)(sizeof(freedv_freqs) / sizeof(freedv_freqs[0])))

static void short_label(void)
{
    int32_t dial = 0, vfo = cparam_i_get(cfg_fg_freq);
    const freq_list_item_t *it = freq_list_base(freedv_freqs, FREQS_N, AUDIO_HZ, vfo, &dial);

    if (it && dial == vfo)
        freq_strip_set_label(it->name, dial);
}

static lv_obj_t *row(lv_obj_t *body, const char *name, lv_obj_t **value)
{
    lv_obj_t *r = lv_obj_create(body);

    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), 40);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *n = lv_label_create(r);
    lv_obj_set_style_text_font(n, &sony_22, 0);
    lv_obj_set_style_text_color(n, TC(text_dim), 0);
    lv_label_set_text(n, name);
    lv_obj_set_pos(n, 0, 6);

    lv_obj_t *v = lv_label_create(r);
    lv_obj_set_style_text_font(v, &sony_22, 0);
    lv_obj_set_style_text_color(v, TC(text), 0);
    lv_label_set_text(v, "-");
    lv_obj_set_pos(v, 220, 6);
    *value = v;
    return r;
}

static void set_text(lv_obj_t *l, const char *t, lv_color_t color)
{
    if (strcmp(t, lv_label_get_text(l)))
        lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, color, 0);
}

static void header_update(void)
{
    char    l[64];
    int32_t f = cparam_i_get(cfg_fg_freq);

    snprintf(l, sizeof(l), "RADE   %.1f kHz", f / 1000.0);
    if (strcmp(l, lv_label_get_text(frame.head_left)))
        lv_label_set_text(frame.head_left, l);
}

static void ui_timer_cb(lv_timer_t *t)
{
    freedv_rx_stats_t s;
    char              b[96];

    (void)t;
    header_update();
    freedv_rx_get_stats(&s);

    if (!started) {
        set_text(lbl_sync, freedv_rx_available() ? "decoder did not start" : "not built into this image", TC(warn));
        return;
    }
    set_text(lbl_sync, s.sync ? "SYNC" : "searching", s.sync ? TC(toggle_on) : TC(text_dim));
    snprintf(b, sizeof(b), s.sync ? "%.1f dB" : "-", s.snr_db);
    set_text(lbl_snr, b, TC(text));
    snprintf(b, sizeof(b), s.sync ? "%+.0f Hz" : "-", s.foff_hz);
    set_text(lbl_foff, b, TC(text));
    snprintf(b, sizeof(b), "%d %%%s", s.level_pct, s.level_pct >= 95 ? "   too loud" : "");
    set_text(lbl_level, b, s.level_pct >= 95 ? TC(warn) : TC(text));
    set_text(lbl_audio, s.audio_out ? "Bluetooth headphones" : "none - connect Bluetooth headphones",
             s.audio_out ? TC(toggle_on) : TC(warn));
    snprintf(b, sizeof(b), "input %u   speech %u   gaps %u", (unsigned)s.in_dropped, (unsigned)s.speech_dropped,
             (unsigned)s.underruns);
    set_text(lbl_loss, b, (s.in_dropped || s.speech_dropped) ? TC(warn) : TC(text_dim));
    snprintf(b, sizeof(b), "decode %d ms   synth %d ms   (of 120)", s.decode_ms_max, s.synth_ms_max);
    set_text(lbl_cpu, b, (s.decode_ms_max > 100 || s.synth_ms_max > 100) ? TC(warn) : TC(text_dim));
}

static void app_open(lv_obj_t *parent)
{
    dialog.obj = dialog_init(parent);
    dsp_set_waterfall_enabled(false);
    dsp_set_spectrum_enabled(false);
    app_frame_create(&frame, dialog.obj);
    lv_label_set_text(frame.head_right, "RX");

    lv_obj_t *body = lv_obj_create(dialog.obj);

    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    row(body, "Sync", &lbl_sync);
    row(body, "SNR", &lbl_snr);
    row(body, "Offset", &lbl_foff);
    row(body, "Input level", &lbl_level);
    row(body, "Speech to", &lbl_audio);
    row(body, "Lost", &lbl_loss);
    row(body, "CPU", &lbl_cpu);
    lv_obj_set_style_text_font(lbl_sync, &sony_22, 0);

    lv_label_set_text(frame.status, "RADE V1, receive only   Filter wide (2.4 kHz+), AGC and NR off   AF 0 or speaker off");

    /* The volume knob is not seen by the main screen while an app is open */
    lv_group_add_obj(keyboard_group, body);
    lv_obj_add_event_cb(body, body_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(body);
    lv_group_set_editing(lv_obj_get_group(body), true);

    started = freedv_rx_start();
    ui_timer = lv_timer_create(ui_timer_cb, UI_TICK_MS, NULL);
    ui_timer_cb(ui_timer);
    short_label();
}

static void app_close(void)
{
    freq_list_close();
    lv_timer_del(ui_timer);
    ui_timer = NULL;
    freedv_rx_stop();       /* the audio callback has left when this returns */
    started = false;
    lbl_sync = lbl_snr = lbl_foff = lbl_level = lbl_audio = lbl_loss = lbl_cpu = NULL;
    dsp_set_waterfall_enabled(true);
    dsp_set_spectrum_enabled(true);
}

static void audio_in(unsigned int n, float *samples)
{
    freedv_rx_put_audio(n, samples);
}

static void body_key_cb(lv_event_t *e)
{
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}

static void freq_cb(button_data_t *data)
{
    (void)data;
    if (freq_list_is_open())
        freq_list_close();
    else if (dialog.obj)
        freq_list_open(dialog.obj, "RADE - FreeDV frequencies", freedv_freqs, FREQS_N, AUDIO_HZ, short_label);
}

static void close_btn_cb(button_data_t *btn) { (void)btn; dialog_destruct(); }
