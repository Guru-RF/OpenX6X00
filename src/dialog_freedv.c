// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - RADE app: FreeDV's RADE V1. Receive (src/freedv_rx.c) and "record, then send"
// (src/freedv_tx.c): Rec records the hand mic and encodes it, Send plays the over into the transmitter
// through tx_player at RADE's own power. The receiver is stopped meanwhile. The speech goes to Bluetooth
// headphones and to the TCI clients (the browser's Panel and Remote pages too): the base plays no app audio on
// the speaker while it receives, so the app opens only with one of them there (dialog_freedv_can_open).

#include "dialog_freedv.h"
#include "freedv_rx.h"
#include "freedv_tx.h"
#include "tx_player.h"
#include "tci_server.h"
#include "btaudio.h"
#include "msg_box.h"
#include "params/params.h"
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

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define UI_TICK_MS 200
#define ROW_H      30
/* The RADE carriers sit at 750..2200 Hz above the dial frequency (rade_ofdm.c: 30 carriers of
 * 50 Hz around 1500 Hz): USB or LSB alike, the list's frequencies are the dial's */
#define AUDIO_HZ 0

typedef enum { ST_RX, ST_REC, ST_READY, ST_TX } st_t;

static app_frame_t frame;
static lv_timer_t *ui_timer;
static bool        started;                 /* the receiver runs */
static atomic_int  state = ST_RX;           /* read by the audio thread (dialog_freedv_is_recording) */

/* The send: tx_player_stream blocks until the end, so a thread of its own */
static pthread_t   tx_thr;
static bool        tx_thr_live;
static atomic_bool tx_abort, tx_finished, tx_sent;

static lv_obj_t *lbl_sync, *lbl_snr, *lbl_level, *lbl_audio, *lbl_loss, *lbl_cpu;

static void app_open(lv_obj_t *parent);
static void app_close(void);
static void audio_in(unsigned int n, float *samples);
static void body_key_cb(lv_event_t *e);
static void freq_cb(button_data_t *data);
static void rec_cb(button_data_t *data);
static void send_cb(button_data_t *data);
static void discard_cb(button_data_t *data);
static void close_btn_cb(button_data_t *btn);
static void pwr_cb(button_data_t *data);
static const char *rec_label(void);
static const char *pwr_label(void);

static button_data_t btn_freq    = {.type = BTN_TEXT, .label = "Freq.", .press = freq_cb};
static button_data_t btn_rec     = {.type = BTN_TEXT_FN, .label_fn = rec_label, .press = rec_cb};
static button_data_t btn_send    = {.type = BTN_TEXT, .label = "Send", .press = send_cb};
static button_data_t btn_discard = {.type = BTN_TEXT, .label = "Discard", .press = discard_cb};
static button_data_t btn_close   = {.type = BTN_TEXT, .label = "Close", .press = close_btn_cb};
static button_data_t btn_close2  = {.type = BTN_TEXT, .label = "Close", .press = close_btn_cb};
static button_data_t btn_pwr     = {.type = BTN_TEXT_FN, .label_fn = pwr_label, .press = pwr_cb};  /* RADE's own */

static buttons_page_t page_1, page_2;

static button_data_t btn_page1 = {.type = BTN_TEXT, .label = "(RADE 1:2)", .press = button_next_page_cb,
                                  .hold = button_prev_page_cb, .next = &page_2, .prev = &page_2};
static button_data_t btn_page2 = {.type = BTN_TEXT, .label = "(RADE 2:2)", .press = button_next_page_cb,
                                  .hold = button_prev_page_cb, .next = &page_1, .prev = &page_1};

static buttons_page_t page_1 = {.items = {&btn_page1, &btn_freq, &btn_rec, &btn_send, &btn_close}};
static buttons_page_t page_2 = {.items = {&btn_page2, &btn_pwr, &btn_discard, &btn_close2}};

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
    lv_obj_set_size(r, lv_pct(100), ROW_H);   /* six of them in the app's body on both radios' screen */
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *n = lv_label_create(r);
    lv_obj_set_style_text_font(n, &sony_22, 0);
    lv_obj_set_style_text_color(n, TC(text_dim), 0);
    lv_label_set_text(n, name);
    lv_obj_set_pos(n, 0, 2);

    lv_obj_t *v = lv_label_create(r);
    lv_obj_set_style_text_font(v, &sony_22, 0);
    lv_obj_set_style_text_color(v, TC(text), 0);
    lv_label_set_text(v, "-");
    lv_obj_set_pos(v, 200, 2);
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

static const char *rec_label(void)
{
    int st = atomic_load(&state);

    return st == ST_REC || st == ST_TX ? "Stop" : "Rec";
}

bool dialog_freedv_is_recording(void)
{
    return atomic_load(&state) == ST_REC;
}

static void buttons_update(void)
{
    if (btn_rec.disp_btn)
        buttons_refresh(&btn_rec);
}

/* The receiver back (after a recording, a send or a discard) */
static void rx_resume(void)
{
    atomic_store(&state, ST_RX);
    if (!started)
        started = freedv_rx_start();
    lv_label_set_text(frame.head_right, "RX");
    buttons_update();
}

static void tx_join(void)
{
    if (tx_thr_live) {
        pthread_join(tx_thr, NULL);
        tx_thr_live = false;
    }
}

/* Recording ends: the hand mic off, the over is encoded in the background */
static void rec_end(void)
{
    freedv_tx_stop();
    radio_set_play_mode(AUDIO_PLAY_OFF);
    atomic_store(&state, ST_READY);
    lv_label_set_text(frame.head_right, "READY");
    buttons_update();
}

/* The over and the encoder go; the receiver stays off until rx_resume() */
static void over_drop(void)
{
    tx_join();
    freedv_tx_discard();
}

static void show_tx_state(void)
{
    freedv_tx_stats_t t;
    char              b[96];
    int               st = atomic_load(&state);

    freedv_tx_get_stats(&t);
    if (st == ST_REC) {
        snprintf(b, sizeof(b), "REC  %.1f s of %d", t.rec_ms / 1000.0, FREEDV_TX_MAX_S);
        set_text(lbl_sync, b, TC(tx_bg));
    } else if (st == ST_TX) {
        set_text(lbl_sync, "TRANSMITTING", TC(tx_bg));
    } else {
        if (t.done)
            snprintf(b, sizeof(b), "over ready  %.1f s", t.over_ms / 1000.0);
        else
            snprintf(b, sizeof(b), "encoding  %d %%", t.rec_ms ? t.encoded_ms * 100 / t.rec_ms : 100);
        set_text(lbl_sync, b, t.done ? TC(toggle_on) : TC(warn));
    }
    set_text(lbl_snr, "-", TC(text_dim));
    if (st == ST_REC) {
        snprintf(b, sizeof(b), "%d %%%s", t.level_pct, t.level_pct >= 95 ? "   too loud" : "");
        set_text(lbl_level, b, t.level_pct >= 95 ? TC(warn) : TC(text));
    }
    set_text(lbl_audio, st == ST_TX ? "the transmitter" : "hand mic only", TC(text));
    set_text(lbl_loss, st == ST_REC ? "do not press the mic's PTT" : "-", TC(text_dim));
    snprintf(b, sizeof(b), "encode %d ms   (of 120)", t.encode_ms_max);
    set_text(lbl_cpu, b, t.encode_ms_max > 100 ? TC(warn) : TC(text_dim));

    if (st == ST_REC) {
        /* The radio keyed under the recording (the mic's PTT, CAT): it would send the voice itself, not RADE */
        if (radio_get_state() != RADIO_RX) {
            rec_end();
            over_drop();
            rx_resume();
            msg_update_text_fmt("RADE: the radio keyed, recording dropped");
        } else if (t.full) {
            rec_end();
        }
    } else if (st == ST_TX && atomic_load(&tx_finished)) {
        tx_join();
        atomic_store(&state, ST_READY);
        freedv_tx_rewind();
        lv_label_set_text(frame.head_right, "READY");
        buttons_update();
        if (!atomic_load(&tx_sent))
            msg_update_text_fmt("RADE: not sent");
    }
}

static void ui_timer_cb(lv_timer_t *t)
{
    freedv_rx_stats_t s;
    char              b[96];

    (void)t;
    header_update();
    if (atomic_load(&state) != ST_RX) {
        show_tx_state();
        return;
    }
    freedv_rx_get_stats(&s);

    if (!started) {
        set_text(lbl_sync, freedv_rx_available() ? "decoder did not start" : "not built into this image", TC(warn));
        return;
    }
    set_text(lbl_sync, s.sync ? "SYNC" : "searching", s.sync ? TC(toggle_on) : TC(text_dim));
    if (s.sync)
        snprintf(b, sizeof(b), "%.1f dB   %+.0f Hz", s.snr_db, s.foff_hz);
    else
        snprintf(b, sizeof(b), "-");
    set_text(lbl_snr, b, TC(text));
    snprintf(b, sizeof(b), "%d %%%s", s.level_pct, s.level_pct >= 95 ? "   too loud" : "");
    set_text(lbl_level, b, s.level_pct >= 95 ? TC(warn) : TC(text));
    int tci = tci_server_audio_clients();

    if (s.audio_out && tci)
        snprintf(b, sizeof(b), "Bluetooth headphones, TCI / browser (%d)", tci);
    else if (s.audio_out)
        snprintf(b, sizeof(b), "Bluetooth headphones");
    else if (tci)
        snprintf(b, sizeof(b), "TCI / browser (%d)", tci);
    else
        snprintf(b, sizeof(b), "none - headphones, TCI or the browser");
    set_text(lbl_audio, b, s.audio_out || tci ? TC(toggle_on) : TC(warn));
    snprintf(b, sizeof(b), "input %u   speech %u   gaps %u%s%s", (unsigned)s.in_dropped, (unsigned)s.speech_dropped,
             (unsigned)s.underruns, s.callsign[0] ? "   last over: " : "", s.callsign);
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
    lv_obj_set_style_pad_hor(body, 12, 0);
    lv_obj_set_style_pad_ver(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    row(body, "Sync", &lbl_sync);
    row(body, "SNR, offset", &lbl_snr);
    row(body, "Input level", &lbl_level);
    row(body, "Speech to", &lbl_audio);
    row(body, "Lost", &lbl_loss);
    row(body, "CPU", &lbl_cpu);
    lv_obj_set_style_text_font(lbl_sync, &sony_22, 0);

    lv_label_set_text(frame.status, "RADE V1   Rec: hand mic only, then Send   Filter wide, AGC and NR off");

    /* The volume knob is not seen by the main screen while an app is open */
    lv_group_add_obj(keyboard_group, body);
    lv_obj_add_event_cb(body, body_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(body);
    lv_group_set_editing(lv_obj_get_group(body), true);

    atomic_store(&state, ST_RX);
    freedv_rx_set_tap(tci_server_speech_write);
    tci_server_speech_on(true);
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
    if (atomic_load(&state) == ST_REC)
        radio_set_play_mode(AUDIO_PLAY_OFF);
    atomic_store(&tx_abort, true);      /* a send in progress ends at the next block */
    atomic_store(&state, ST_RX);
    tx_join();
    freedv_tx_discard();
    freedv_rx_stop();       /* the audio callback has left when this returns */
    started = false;
    tci_server_speech_on(false);
    lbl_sync = lbl_snr = lbl_level = lbl_audio = lbl_loss = lbl_cpu = NULL;
    dsp_set_waterfall_enabled(true);
    dsp_set_spectrum_enabled(true);
}

static void audio_in(unsigned int n, float *samples)
{
    if (atomic_load(&state) == ST_RX)
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

static bool abort_cb(void *arg)
{
    (void)arg;
    return atomic_load(&tx_abort);
}

/* At RADE's own power, not the radio's setting or the level the other apps share (tx_player_set_pwr): the power
 * reading follows the OFDM signal's average, and its peaks go several dB higher */
static void *tx_worker(void *arg)
{
    (void)arg;
    tx_player_set_pwr(param_i_get(cfg_rade_pwr) / 10.0f);
    bool ok = tx_player_stream(freedv_tx_read, NULL, TX_PLAYER_AUDIO_HZ, tx_player_base_gain_offset(), abort_cb, NULL);

    tx_player_set_pwr(0.0f);
    atomic_store(&tx_sent, ok);
    atomic_store(&tx_finished, true);
    return NULL;
}

static void rec_cb(button_data_t *data)
{
    (void)data;
    switch (atomic_load(&state)) {
    case ST_REC:
        rec_end();
        break;
    case ST_TX:
        atomic_store(&tx_abort, true);
        break;
    default:
        if (!freedv_tx_available()) {
            msg_update_text_fmt("RADE: not built into this image");
            return;
        }
        if (radio_get_state() != RADIO_RX) {
            msg_update_text_fmt("RADE: the radio is transmitting");
            return;
        }
        if (strlen(params.callsign.x) > FREEDV_CALL_MAX)
            msg_update_text_fmt("RADE: callsign sent as the first %d characters", FREEDV_CALL_MAX);
        if (atomic_load(&state) == ST_READY)
            over_drop();
        freq_list_close();
        freedv_rx_stop();           /* the mic's audio is not the receiver's */
        started = false;
        if (!freedv_tx_begin(params.callsign.x)) {
            msg_update_text_fmt("RADE: cannot start the encoder");
            rx_resume();
            return;
        }
        radio_set_play_mode(AUDIO_PLAY_VOICE_REC);      /* the hand mic into Linux, as the voice messages */
        atomic_store(&state, ST_REC);
        lv_label_set_text(frame.head_right, "REC");
        buttons_update();
        break;
    }
}

static void send_cb(button_data_t *data)
{
    freedv_tx_stats_t t;

    (void)data;
    if (atomic_load(&state) != ST_READY) {
        msg_update_text_fmt("RADE: Rec first");
        return;
    }
    freedv_tx_get_stats(&t);
    if (!t.done) {
        msg_update_text_fmt("RADE: still encoding");
        return;
    }
    if (!params.callsign.x[0]) {
        msg_update_text_fmt("RADE: set your callsign first (APP > Callsign)");
        return;
    }
    freedv_tx_rewind();
    atomic_store(&tx_abort, false);
    atomic_store(&tx_finished, false);
    atomic_store(&tx_sent, false);
    if (pthread_create(&tx_thr, NULL, tx_worker, NULL) != 0) {
        msg_update_text_fmt("RADE: cannot start the send");
        return;
    }
    tx_thr_live = true;
    atomic_store(&state, ST_TX);
    lv_label_set_text(frame.head_right, "TX");
    buttons_update();
}

static void discard_cb(button_data_t *data)
{
    (void)data;
    if (atomic_load(&state) == ST_READY) {
        over_drop();
        rx_resume();
    }
}

static void freq_cb(button_data_t *data)
{
    (void)data;
    if (atomic_load(&state) != ST_RX)
        return;
    if (freq_list_is_open())
        freq_list_close();
    else if (dialog.obj)
        freq_list_open(dialog.obj, "RADE - FreeDV frequencies", freedv_freqs, FREQS_N, AUDIO_HZ, short_label);
}

static void close_btn_cb(button_data_t *btn) { (void)btn; dialog_destruct(); }

static const char *pwr_label(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), "Power\n%g W", param_i_get(cfg_rade_pwr) / 10.0);
    return buf;
}

/* RADE's own power: the next level up, round to the lowest. Takes effect at the next Send */
static void pwr_cb(button_data_t *data)
{
    static const int levels[] = {5, 10, 20, 30};
    int              cur = param_i_get(cfg_rade_pwr);
    size_t           k   = 0;

    while (k < sizeof(levels) / sizeof(levels[0]) && levels[k] <= cur)
        k++;
    param_i_set(cfg_rade_pwr, k < sizeof(levels) / sizeof(levels[0]) ? levels[k] : levels[0]);
    buttons_refresh(data);
}

/* Somewhere to hear the speech: Bluetooth headphones (with Mute speaker on, as audio_play_bt_begin wants them) or a
 * TCI client (the VFO-Knob, the browser's Panel or Remote page) */
bool dialog_freedv_can_open(void)
{
    char sink[128];

    return btaudio_hp_sink(sink, sizeof(sink)) || tci_server_clients() > 0;
}
