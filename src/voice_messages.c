/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  Voice keyer: record messages from the mic, listen to them, send them
 *  on air once or as an automatic call. One page for the usual work (Rec,
 *  Listen, TX, Auto call), a second one to manage the messages (Interval,
 *  Rename, Delete, Close).
 */

#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sndfile.h>
#include <dirent.h>
#include <pthread.h>
#include <math.h>

#include "audio.h"
#include "cfg/cfg_api.h"
#include "app_window.h"
#include "voice_messages.h"
#include "gui_styles.h"
#include "params/params.h"
#include "gui_events.h"
#include "info_bar.h"
#include "key_input.h"
#include "text_popup.h"
#include "msg_box.h"
#include "radio.h"
#include "theme.h"

#define PLAY_CHUNK   1024
#define MAX_ITEMS    64
#define NAME_LEN     40
#define REC_MAX_S    60
#define TIMER_MS     100
#define CONFIRM_MS   3000
#define LEAD_IN_US   100000
#define PERIOD_MIN   3
#define PERIOD_MAX   300
#define OWN_TX_MS    1000

#define LIST_X       10
#define LIST_Y       40
#define LIST_W       470
#define LIST_H       (DIALOG_H - 86)
#define SIDE_X       494
#define SIDE_W       290

#define COLOR_HEAD   theme_hex(TC_text_head)
#define COLOR_DIM    theme_hex(TC_text_dim)
#define COLOR_SEL_BG theme_hex(TC_sel_bg)
#define COLOR_SEL    theme_hex(TC_sel_text)

typedef struct {
    char  name[NAME_LEN + 1];
    float secs;
} item_t;

#ifndef VOICE_MSG_PATH
#define VOICE_MSG_PATH "/mnt/msg"
#endif
static const char *path = VOICE_MSG_PATH;

static item_t  items[MAX_ITEMS];
static int     count;
static int     sel;

static volatile msg_voice_state_t state = MSG_VOICE_OFF;

/* Recording, written by the audio thread */
static pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;
static SNDFILE        *rec_file;
static char            rec_name[NAME_LEN + 1];
static volatile long   rec_frames;
static volatile int    rec_peak;
static volatile int    rec_max;         /* the loudest so far: none means no hand mic */
static bool            rec_warned;

/* The hand mic input carries the radio's own hum (multiples of 33-40 Hz) and two whistles,
 * 781 Hz and 9.6 kHz (SQ7ACP; measured on an idle input): the message keeps 250-3000 Hz, all
 * that SSB sends, with a narrow notch at 781 Hz */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;

#define REC_HP       0
#define REC_LP       2
#define REC_NOTCH    4
#define REC_STAGES   5
static biquad_t rec_filter[REC_STAGES];

/* RBJ cookbook: 0 high pass, 1 low pass, 2 notch */
static void biquad_set(biquad_t *f, int kind, float hz, float q) {
    float w = 2.0f * (float)M_PI * hz / AUDIO_CAPTURE_RATE, c = cosf(w), a = sinf(w) / (2.0f * q);
    float a0 = 1.0f + a;

    if (kind == 0) {
        f->b0 = (1.0f + c) / 2.0f; f->b1 = -(1.0f + c); f->b2 = f->b0;
    } else if (kind == 1) {
        f->b0 = (1.0f - c) / 2.0f; f->b1 = 1.0f - c; f->b2 = f->b0;
    } else {
        f->b0 = 1.0f; f->b1 = -2.0f * c; f->b2 = 1.0f;
    }
    f->b0 /= a0; f->b1 /= a0; f->b2 /= a0;
    f->a1 = -2.0f * c / a0;
    f->a2 = (1.0f - a) / a0;
    f->z1 = f->z2 = 0.0f;
}

static float biquad_run(biquad_t *f, float x) {
    float y = f->b0 * x + f->z1;

    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static void rec_filter_init(void) {
    for (int i = 0; i < 2; i++) {
        biquad_set(&rec_filter[REC_HP + i], 0, 250.0f, 0.707f);
        biquad_set(&rec_filter[REC_LP + i], 1, 3000.0f, 0.707f);
    }
    biquad_set(&rec_filter[REC_NOTCH], 2, 781.0f, 10.0f);
}

/* Playback worker: listen locally, or send on air (once or as an auto call) */
static pthread_t     worker;
static bool          worker_started;
static volatile bool worker_done;
static volatile bool stop_req;
static volatile bool on_air;
static volatile bool pausing;
static volatile long play_pos, play_total;
static volatile int  pause_left;
static volatile uint32_t air_off_tick;
static bool          job_tx;
static bool          job_loop;
static bool          adjusting;
static char          play_path[128];
static int16_t       samples_buf[PLAY_CHUNK];

static uint32_t delete_armed;

static lv_obj_t   *list, *head_right, *state_lbl, *name_lbl, *bar, *info_lbl, *hint;
static lv_timer_t *timer;
static bool        renaming;

static void window_open(lv_obj_t *parent);
static void window_close(void);
static void rec_cb(button_data_t *btn);
static void listen_cb(button_data_t *btn);
static void tx_btn_cb(button_data_t *btn);
static void close_cb(button_data_t *btn);
static void auto_cb(button_data_t *btn);
static void interval_cb(button_data_t *btn);
static void rename_cb(button_data_t *btn);
static void delete_cb(button_data_t *btn);
static const char *rec_label(void);
static const char *listen_label(void);
static const char *tx_label(void);
static const char *auto_label(void);
static const char *interval_label(void);

static button_data_t btn_p1 = {.type = BTN_TEXT, .label = "(MSG 1:2)", .press = button_next_page_cb};
static button_data_t btn_rec = {.type = BTN_TEXT_FN, .label_fn = rec_label, .press = rec_cb};
static button_data_t btn_listen = {.type = BTN_TEXT_FN, .label_fn = listen_label, .press = listen_cb};
static button_data_t btn_tx = {.type = BTN_TEXT_FN, .label_fn = tx_label, .press = tx_btn_cb};
static button_data_t btn_auto = {.type = BTN_TEXT_FN, .label_fn = auto_label, .press = auto_cb};

static button_data_t btn_p2 = {.type = BTN_TEXT, .label = "(MSG 2:2)", .press = button_next_page_cb};
static button_data_t btn_interval = {.type = BTN_TEXT_FN, .label_fn = interval_label, .press = interval_cb};
static button_data_t btn_rename = {.type = BTN_TEXT, .label = "Rename", .press = rename_cb};
static button_data_t btn_close = {.type = BTN_TEXT, .label = "Close", .press = close_cb};
static button_data_t btn_delete = {.type = BTN_TEXT, .label = "Delete", .press = delete_cb};

static buttons_page_t page_1 = {{&btn_p1, &btn_rec, &btn_listen, &btn_tx, &btn_auto}};
static buttons_page_t page_2 = {{&btn_p2, &btn_interval, &btn_rename, &btn_delete, &btn_close}};

static dialog_t dialog = { .construct_cb = window_open, .destruct_cb = window_close, .btn_page = &page_1 };

dialog_t *dialog_msg_voice = &dialog;

/* Messages */

static int cmp_item(const void *a, const void *b) {
    return strcasecmp(((const item_t *)a)->name, ((const item_t *)b)->name);
}

static void file_path(char *dst, size_t size, const char *name) {
    snprintf(dst, size, "%s/%s.mp3", path, name);
}

static float file_secs(const char *name) {
    char    p[128];
    SF_INFO info;
    memset(&info, 0, sizeof(info));
    file_path(p, sizeof(p), name);
    SNDFILE *f = sf_open(p, SFM_READ, &info);
    if (!f) return 0;
    sf_close(f);
    return info.samplerate > 0 ? (float)info.frames / info.samplerate : 0;
}

static void show_list(void) {
    char text[16];
    lv_table_set_row_cnt(list, count > 0 ? count : 1);
    if (count == 0) {
        lv_table_set_cell_value(list, 0, 0, "No messages yet: press Rec");
        lv_table_set_cell_value(list, 0, 1, "");
    }
    for (int i = 0; i < count; i++) {
        lv_table_set_cell_value(list, i, 0, items[i].name);
        snprintf(text, sizeof(text), "%.1f s", items[i].secs);
        lv_table_set_cell_value(list, i, 1, text);
    }
    lv_label_set_text_fmt(head_right, "%i message%s", count, count == 1 ? "" : "s");
    lv_obj_invalidate(list);
}

/* Reads /mnt/msg; select names that entry afterwards when given. */
static void load_items(const char *select) {
    DIR           *dp = opendir(path);
    struct dirent *ep;

    count = 0;
    if (dp) {
        while ((ep = readdir(dp)) != NULL && count < MAX_ITEMS) {
            size_t n = strlen(ep->d_name);
            if (n < 5 || n - 4 > NAME_LEN || strcasecmp(ep->d_name + n - 4, ".mp3") != 0) continue;
            memcpy(items[count].name, ep->d_name, n - 4);
            items[count].name[n - 4] = '\0';
            items[count].secs        = file_secs(items[count].name);
            count++;
        }
        closedir(dp);
    }
    qsort(items, count, sizeof(items[0]), cmp_item);

    if (select)
        for (int i = 0; i < count; i++)
            if (strcmp(items[i].name, select) == 0) sel = i;
    if (sel >= count) sel = count - 1;
    if (sel < 0) sel = 0;
    show_list();
}

static const char *sel_name(void) {
    return count > 0 ? items[sel].name : NULL;
}

/* Playback worker */

static int interval_s(void) {
    int v = params.voice_msg_period;
    return v < PERIOD_MIN ? PERIOD_MIN : (v > PERIOD_MAX ? PERIOD_MAX : v);
}

static bool play_file(void) {
    SF_INFO info;
    memset(&info, 0, sizeof(info));
    SNDFILE *f = sf_open(play_path, SFM_READ, &info);
    if (!f) return false;

    play_total = info.frames;
    play_pos   = 0;
    while (!stop_req) {
        int n = (int)sf_read_short(f, samples_buf, PLAY_CHUNK);
        if (n <= 0) break;
        int gain = param_i_get(cfg_voice_msg_gain);   /* read each block: Field Ops changes it while the CQ plays */
        if (gain) audio_gain_db(samples_buf, n, gain, samples_buf);
        audio_play(samples_buf, n);
        play_pos += n;
    }
    sf_close(f);

    /* Stop: drop the queued audio instead of sending it. */
    if (stop_req) audio_play_flush();
    else audio_play_wait();
    return true;
}

static void *worker_cb(void *arg) {
    (void)arg;
    while (true) {
        bool ok;
        if (job_tx) {
            on_air = true;
            radio_set_modem(true);
            /* Stop during the lead-in: play_file drops the audio at once */
            if (!stop_req) usleep(LEAD_IN_US);
            ok = play_file();
            radio_set_modem(false);
            air_off_tick = lv_tick_get();
            on_air       = false;
        } else {
            /* Preview: on the Bluetooth headphones while they replace the speaker */
            bool bt = audio_play_bt_begin();
            if (!bt) radio_set_play_mode(AUDIO_PLAY_ON);
            ok = play_file();
            if (bt) audio_play_bt_end();
            else radio_set_play_mode(AUDIO_PLAY_OFF);
        }
        if (!ok || !job_tx || !job_loop || stop_req) break;

        pausing = true;
        for (int t = interval_s() * 10; t > 0 && !stop_req; t--) {
            pause_left = (t + 9) / 10;
            usleep(100000);
        }
        pausing = false;
        if (stop_req) break;
    }
    worker_done = true;
    return NULL;
}

static void start_play_name(bool tx, bool loop, const char *name) {
    if (state != MSG_VOICE_OFF || !name) return;
    file_path(play_path, sizeof(play_path), name);
    job_tx         = tx;
    job_loop       = loop;
    adjusting      = false;
    stop_req       = false;
    worker_done    = false;
    play_pos       = 0;
    play_total     = 0;
    state          = MSG_VOICE_PLAY;
    worker_started = pthread_create(&worker, NULL, worker_cb, NULL) == 0;
    if (!worker_started) state = MSG_VOICE_OFF;
}

static void start_play(bool tx, bool loop) {
    start_play_name(tx, loop, sel_name());
}

static void stop_play(void) {
    stop_req = true;
}

/* Joins a finished worker; the UI timer and the dialog end call it. */
static void reap_worker(bool wait) {
    if (!worker_started) return;
    if (!wait && !worker_done) return;
    stop_req = wait || stop_req;
    pthread_join(worker, NULL);
    worker_started = false;
    state          = MSG_VOICE_OFF;
}

/* Recording */

static void next_rec_name(char *dst) {
    char p[128];
    for (int n = count + 1;; n++) {
        snprintf(dst, NAME_LEN + 1, "Message %i", n);
        file_path(p, sizeof(p), dst);
        if (access(p, F_OK) != 0) return;
    }
}

/* name: NULL for the next "Message N" */
static void start_rec(const char *name) {
    char    p[128];
    SF_INFO info;

    if (name) snprintf(rec_name, sizeof(rec_name), "%s", name);
    else next_rec_name(rec_name);
    file_path(p, sizeof(p), rec_name);
    memset(&info, 0, sizeof(info));
    info.samplerate = AUDIO_CAPTURE_RATE;
    info.channels   = 1;
    info.format     = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;

    SNDFILE *f = sf_open(p, SFM_WRITE, &info);
    if (!f) {
        msg_update_text_fmt("Cannot create %s", p);
        return;
    }
    radio_set_play_mode(AUDIO_PLAY_VOICE_REC);
    pthread_mutex_lock(&rec_mutex);
    rec_filter_init();
    rec_file   = f;
    rec_frames = 0;
    rec_peak   = 0;
    rec_max    = 0;
    rec_warned = false;
    state      = MSG_VOICE_RECORD;
    pthread_mutex_unlock(&rec_mutex);
}

static void stop_rec(void) {
    pthread_mutex_lock(&rec_mutex);
    state = MSG_VOICE_OFF;
    if (rec_file) sf_close(rec_file);
    rec_file = NULL;
    pthread_mutex_unlock(&rec_mutex);
    radio_set_play_mode(AUDIO_PLAY_OFF);
    if (dialog.run) load_items(rec_name);   /* the list exists only with the window open */
}

/* Called from the audio thread while recording */
void dialog_msg_voice_put_audio_samples(size_t nsamples, int16_t *samples) {
    int16_t out[nsamples];
    int     peak = 0;

    pthread_mutex_lock(&rec_mutex);
    if (state == MSG_VOICE_RECORD && rec_file) {
        for (size_t i = 0; i < nsamples; i++) {
            float y = samples[i];
            for (int k = 0; k < REC_STAGES; k++) y = biquad_run(&rec_filter[k], y);
            out[i] = (int16_t)LV_CLAMP(-32767.0f, y, 32767.0f);
            if (abs(out[i]) > peak) peak = abs(out[i]);
        }
        sf_write_short(rec_file, out, nsamples);
        rec_frames += nsamples;
        rec_peak = peak;
        if (rec_frames > AUDIO_CAPTURE_RATE / 2 && peak > rec_max) rec_max = peak;   /* past the switch click */
    }
    pthread_mutex_unlock(&rec_mutex);
}

/* 3 s of almost nothing (under -40 dBFS): the radio records only from the hand mic */
static void rec_check_quiet(void) {
    if (state != MSG_VOICE_RECORD || rec_warned || rec_frames < 3L * AUDIO_CAPTURE_RATE) return;
    rec_warned = true;
    if (rec_max < 330) msg_update_text_fmt("No audio: recording uses the hand mic");
}

msg_voice_state_t dialog_msg_voice_get_state() {
    return state;
}

/* A named message without the window (Field Ops' voice CQ) */

bool msg_voice_rec_toggle(const char *name) {
    if (state == MSG_VOICE_RECORD) {
        stop_rec();
        return false;
    }
    if (state == MSG_VOICE_OFF) start_rec(name);
    return state == MSG_VOICE_RECORD;
}

bool msg_voice_send_toggle(const char *name) {
    char p[128];

    if (state == MSG_VOICE_PLAY) {
        stop_play();
        return false;
    }
    file_path(p, sizeof(p), name);
    if (access(p, R_OK) != 0) return false;
    start_play_name(true, false, name);
    return state == MSG_VOICE_PLAY;
}

bool msg_voice_exists(const char *name) {
    char p[128];

    file_path(p, sizeof(p), name);
    return access(p, R_OK) == 0;
}

void msg_voice_poll(bool closing) {
    rec_check_quiet();
    if (state == MSG_VOICE_RECORD && (closing || rec_frames >= (long)REC_MAX_S * AUDIO_CAPTURE_RATE)) stop_rec();
    reap_worker(closing);
}

/* Buttons */

static bool listening(void) {
    return state == MSG_VOICE_PLAY && !job_tx;
}

static bool sending(void) {
    return state == MSG_VOICE_PLAY && job_tx;
}

static const char *rec_label(void) {
    return state == MSG_VOICE_RECORD ? "Stop" : "Rec";
}

static const char *listen_label(void) {
    return listening() ? "Stop" : "Listen";
}

static const char *tx_label(void) {
    return sending() ? "Stop" : "TX";
}

static const char *auto_label(void) {
    return sending() && job_loop ? "Auto\nStop" : "Auto\ncall";
}

static const char *interval_label(void) {
    static char buf[24];
    snprintf(buf, sizeof(buf), adjusting ? "Interval\n< %i s >" : "Interval\n%i s", interval_s());
    return buf;
}

static void set_btn(button_data_t *b, bool disabled) {
    if (b->disabled != disabled) buttons_disabled(b, disabled);
}

/* Redraws a button whose label getter changed its text. */
static void relabel(button_data_t *b, const char **last) {
    const char *now = b->label_fn();
    if (now != *last) {
        *last = now;
        if (b->disp_btn) buttons_refresh(b);
    }
}

static void update_buttons(void) {
    bool empty    = count == 0;
    bool rec      = state == MSG_VOICE_RECORD;
    bool idle     = state == MSG_VOICE_OFF;

    set_btn(&btn_rec, !(rec || idle));
    set_btn(&btn_listen, !(listening() || (idle && !empty)));
    set_btn(&btn_tx, !(sending() || (idle && !empty)));
    set_btn(&btn_auto, !(sending() || (idle && !empty)));
    set_btn(&btn_close, rec);
    set_btn(&btn_interval, !idle);
    set_btn(&btn_rename, !idle || empty);
    set_btn(&btn_delete, !idle || empty);
    set_btn(&btn_p1, !idle);
    set_btn(&btn_p2, !idle);

    static const char *rec_l, *listen_l, *tx_l, *auto_l;
    relabel(&btn_rec, &rec_l);
    relabel(&btn_listen, &listen_l);
    relabel(&btn_tx, &tx_l);
    relabel(&btn_auto, &auto_l);
}

/* Status panel */

static void set_state(const char *text, uint32_t bg, uint32_t fg) {
    lv_label_set_text(state_lbl, text);
    lv_obj_set_style_bg_color(state_lbl, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(state_lbl, lv_color_hex(fg), 0);
}

static void update_status(void) {
    const char *name = sel_name();

    if (state == MSG_VOICE_RECORD) {
        long s = rec_frames / AUDIO_CAPTURE_RATE;
        char text[32];
        snprintf(text, sizeof(text), "REC %02li:%02li", s / 60, s % 60);
        set_state(text, theme_hex(TC_err_bg), theme_hex(TC_err));
        lv_label_set_text(name_lbl, rec_name);
        lv_bar_set_value(bar, rec_peak * 100 / 32768, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, (rec_peak > 26000 ? TC(bar_high) : TC(ok_bar)), LV_PART_INDICATOR);
        lv_label_set_text_fmt(info_lbl, "Mic level: keep out of the red\nMax %i s", REC_MAX_S);
        lv_label_set_text(hint, "Stop: save the message");
        return;
    }

    lv_label_set_text(name_lbl, name ? name : "-");
    lv_obj_set_style_bg_color(bar, TC(tx), LV_PART_INDICATOR);
    int pct = play_total > 0 ? (int)(play_pos * 100 / play_total) : 0;

    if (sending()) {
        if (pausing) {
            char text[32];
            snprintf(text, sizeof(text), "AUTO %i s", pause_left);
            set_state(text, theme_hex(TC_chip), theme_hex(TC_warn));
        } else {
            set_state("ON AIR", theme_hex(TC_tx_bg), theme_hex(TC_text));
        }
        lv_bar_set_value(bar, pausing ? 0 : pct, LV_ANIM_OFF);
        if (!job_loop)
            lv_label_set_text(info_lbl, "Single transmission");
        else if (pausing)
            lv_label_set_text_fmt(info_lbl, "Next call in %i s\nInterval %i s", pause_left, interval_s());
        else
            lv_label_set_text_fmt(info_lbl, "Auto call: repeats every %i s", interval_s());
        lv_label_set_text(hint, job_loop ? "Auto call, TX or PTT: stop" : "TX or PTT: stop");
    } else if (listening()) {
        set_state("LISTEN", theme_hex(TC_ok_bg), theme_hex(TC_ok));
        lv_obj_set_style_bg_color(bar, TC(ok_bar), LV_PART_INDICATOR);
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
        lv_label_set_text(info_lbl, "Playing on the speaker only");
        lv_label_set_text(hint, "Listen: stop");
    } else {
        set_state("READY", theme_hex(TC_chip), theme_hex(TC_text_head));
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_label_set_text_fmt(info_lbl, "Auto call every %i s\nInterval: page 2", interval_s());
        lv_label_set_text(hint, adjusting ? "MFK or VOL: interval - press MFK: done"
                          : count ? "MFK: choose - press MFK: listen" : "Rec: record the first message");
    }
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    reap_worker(false);
    rec_check_quiet();
    if (state == MSG_VOICE_RECORD && rec_frames >= (long)REC_MAX_S * AUDIO_CAPTURE_RATE) stop_rec();
    if (delete_armed && lv_tick_elaps(delete_armed) > CONFIRM_MS) delete_armed = 0;
    if (adjusting && buttons_get_cur_page() != &page_2) adjusting = false;
    /* Someone else keyed the radio during the pause (mic PTT): stop. */
    if (pausing && !stop_req && lv_tick_elaps(air_off_tick) > OWN_TX_MS && radio_get_state() == RADIO_TX)
        stop_play();
    update_status();
    update_buttons();
}

/* List drawing: selected row highlighted */

static void list_draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS) return;
    uint32_t r = dsc->id / 2, c = dsc->id % 2;

    dsc->rect_dsc->bg_opa   = LV_OPA_COVER;
    dsc->rect_dsc->bg_color = TC(panel);
    if (c == 1) dsc->label_dsc->color = lv_color_hex(COLOR_DIM);
    if (count == 0) {
        dsc->label_dsc->color = lv_color_hex(COLOR_DIM);
        return;
    }
    if ((int)r == sel) {
        dsc->rect_dsc->bg_color     = lv_color_hex(COLOR_SEL_BG);
        dsc->rect_dsc->border_color = TC(sel_border);
        dsc->rect_dsc->border_width = 2;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM;
        dsc->label_dsc->color       = lv_color_hex(COLOR_SEL);
    }
}

static void move_sel(int step) {
    if (state != MSG_VOICE_OFF || count == 0) return;
    sel += step;
    if (sel < 0) sel = 0;
    if (sel >= count) sel = count - 1;

    lv_table_t *t = (lv_table_t *)list;
    lv_coord_t  y = 0;
    for (int i = 0; i < sel; i++) y += t->row_h[i];
    lv_obj_scroll_to_y(list, y + t->row_h[sel] / 2 - LIST_H / 2, LV_ANIM_OFF);
    lv_obj_invalidate(list);
    delete_armed = 0;
}

/* Keys */

static void change_interval(int step) {
    int v = interval_s() + step;
    if (v < PERIOD_MIN) v = PERIOD_MIN;
    if (v > PERIOD_MAX) v = PERIOD_MAX;
    params_lock();
    params.voice_msg_period = v;
    params_unlock(&params.dirty.voice_msg_period);
    buttons_refresh(&btn_interval);
}

/* MFK or VOL turn the interval while Interval is selected, else the list / volume. */
static void list_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (adjusting && state == MSG_VOICE_OFF) {
        switch (key) {
            case LV_KEY_LEFT:
            case LV_KEY_DOWN:
            case KEY_VOL_LEFT_EDIT:
            case KEY_VOL_LEFT_SELECT:
                change_interval(-1);
                return;
            case LV_KEY_RIGHT:
            case LV_KEY_UP:
            case KEY_VOL_RIGHT_EDIT:
            case KEY_VOL_RIGHT_SELECT:
                change_interval(1);
                return;
            case LV_KEY_ENTER:
                interval_cb(NULL);
                return;
            default:
                break;
        }
    }

    if (key == LV_KEY_LEFT || key == LV_KEY_UP)
        move_sel(-1);
    else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN)
        move_sel(1);
    else if (key == LV_KEY_ENTER)
        listen_cb(NULL);
    else if (key == LV_KEY_ESC && state != MSG_VOICE_RECORD)
        dialog_destruct();          /* not while recording: Rec stops it first */
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}

/* MFK press: the encoder sends a click, not ENTER. */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;
    if (adjusting) interval_cb(NULL);
    else listen_cb(NULL);
}

/* The radio went to TX. Our own keying (on_air, plus a short tail for the
 * late flag from the radio) is ignored; anything else is the operator. */
static void radio_tx_cb(lv_event_t *e) {
    (void)e;
    if (state == MSG_VOICE_PLAY && !on_air && lv_tick_elaps(air_off_tick) > OWN_TX_MS) stop_play();
}

/* Front PTT key (main_screen): stops the call even while we are on air. */
void dialog_msg_voice_ptt_pressed(void) {
    if (state == MSG_VOICE_PLAY) stop_play();
}

/* Button actions */

static void rec_cb(button_data_t *btn) {
    (void)btn;
    if (state == MSG_VOICE_RECORD) stop_rec();
    else if (state == MSG_VOICE_OFF) start_rec(NULL);
    timer_cb(NULL);
}

static void listen_cb(button_data_t *btn) {
    (void)btn;
    if (listening()) stop_play();
    else start_play(false, false);
    timer_cb(NULL);
}

static void tx_btn_cb(button_data_t *btn) {
    (void)btn;
    if (sending()) stop_play();
    else start_play(true, false);
    timer_cb(NULL);
}

static void close_cb(button_data_t *btn) {
    (void)btn;
    if (state != MSG_VOICE_RECORD) dialog_destruct();
}

static void auto_cb(button_data_t *btn) {
    (void)btn;
    if (sending() && job_loop) stop_play();
    else start_play(true, true);
    timer_cb(NULL);
}

/* Selects the interval for MFK / VOL; press again to give the encoders back. */
static void interval_cb(button_data_t *btn) {
    (void)btn;
    if (state != MSG_VOICE_OFF) return;
    adjusting = !adjusting;
    buttons_refresh(&btn_interval);
    timer_cb(NULL);
}

static void delete_cb(button_data_t *btn) {
    (void)btn;
    const char *name = sel_name();
    if (state != MSG_VOICE_OFF || !name) return;

    if (!delete_armed) {
        delete_armed = lv_tick_get();
        msg_update_text_fmt("Press Delete again to delete \"%s\"", name);
        return;
    }
    char p[128];
    file_path(p, sizeof(p), name);
    unlink(p);
    delete_armed = 0;
    msg_update_text_fmt("Deleted \"%s\"", name);
    load_items(NULL);
    timer_cb(NULL);
}

static void rename_end(void) {
    textarea_window_close();
    renaming = false;
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static bool rename_ok(void) {
    char name[NAME_LEN + 1], from[128], to[128];
    snprintf(name, sizeof(name), "%s", textarea_window_get());

    /* No leading or trailing spaces */
    char *s = name;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
    if (n == 0) {
        msg_update_text_fmt("Empty name");
        return false;
    }

    file_path(from, sizeof(from), items[sel].name);
    file_path(to, sizeof(to), s);
    if (strcmp(from, to) != 0) {
        if (access(to, F_OK) == 0) {
            msg_update_text_fmt("\"%s\" already exists", s);
            return false;
        }
        if (rename(from, to) != 0) {
            msg_update_text_fmt("Cannot rename");
            return false;
        }
    }
    char keep[NAME_LEN + 1];
    snprintf(keep, sizeof(keep), "%s", s);
    rename_end();
    load_items(keep);
    return true;
}

static bool rename_cancel(void) {
    rename_end();
    return true;
}

static void rename_cb(button_data_t *btn) {
    (void)btn;
    if (state != MSG_VOICE_OFF || !sel_name() || renaming) return;

    renaming = true;
    /* The text area gets the keys only when the dialog is out of the group. */
    lv_group_remove_obj(dialog.obj);
    textarea_window_open_w_label(rename_ok, rename_cancel, "Name: ");
    lv_obj_t *text = textarea_window_text();
    lv_textarea_set_accepted_chars(text, "0123456789 -_?!.'"
                                         "abcdefghijklmnopqrstuvwxyz"
                                         "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    lv_textarea_set_max_length(text, NAME_LEN);
    textarea_window_set(items[sel].name);
}

/* Dialog */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void window_open(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    btn_p1.next = btn_p1.prev = &page_2;
    btn_p2.next = btn_p2.prev = &page_1;

    lv_obj_t *head = label(dialog.obj, &sony_20, COLOR_HEAD);
    lv_label_set_recolor(head, true);
    lv_label_set_text_fmt(head, "VOICE KEYER (TX) - #%06x %s#", theme_hex(TC_text), info_params_mode_label_get());
    lv_obj_set_pos(head, 16, 10);
    head_right = label(dialog.obj, &sony_20, COLOR_HEAD);
    lv_obj_set_width(head_right, 250);
    lv_obj_set_style_text_align(head_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(head_right, 796 - 16 - 250, 10);

    list = lv_table_create(dialog.obj);
    lv_obj_remove_style(list, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(list, LIST_W, LIST_H);
    lv_obj_set_pos(list, LIST_X, LIST_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, theme_shape.border, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, TC(panel_border), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(list, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(list, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(list, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(list, 10, LV_PART_ITEMS);
    lv_table_set_col_cnt(list, 2);
    lv_table_set_col_width(list, 0, LIST_W - 2 - 110);
    lv_table_set_col_width(list, 1, 110);
    lv_obj_add_event_cb(list, list_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    lv_obj_t *side = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, SIDE_W, LIST_H);
    lv_obj_set_pos(side, SIDE_X, LIST_Y);
    lv_obj_set_style_bg_color(side, TC(side), 0);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(side, TC(panel_border), 0);
    lv_obj_set_style_border_width(side, theme_shape.border, 0);
    lv_obj_set_style_radius(side, 8, 0);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);

    state_lbl = label(side, &sony_28, COLOR_HEAD);
    lv_obj_set_size(state_lbl, SIDE_W - 24, 48);
    lv_obj_set_pos(state_lbl, 12, 12);
    lv_obj_set_style_bg_opa(state_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(state_lbl, 8, 0);
    lv_obj_set_style_text_align(state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(state_lbl, 8, 0);

    name_lbl = label(side, &sony_22, theme_hex(TC_text));
    lv_obj_set_width(name_lbl, SIDE_W - 24);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name_lbl, 12, 76);

    bar = lv_bar_create(side);
    lv_obj_set_size(bar, SIDE_W - 24, 14);
    lv_obj_set_pos(bar, 12, 116);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, TC(row), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    info_lbl = label(side, &sony_18, COLOR_DIM);
    lv_obj_set_width(info_lbl, SIDE_W - 24);
    lv_obj_set_pos(info_lbl, 12, 146);

    hint = label(dialog.obj, &sony_18, COLOR_DIM);
    lv_obj_set_pos(hint, 16, DIALOG_H - 32);

    lv_obj_add_event_cb(dialog.obj, radio_tx_cb, EVENT_RADIO_TX, NULL);
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, list_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    mkdir(path, 0755);
    sel       = 0;
    adjusting = false;
    load_items(NULL);
    timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
    timer_cb(NULL);
}

static void window_close(void) {
    /* Leaving any other way (another app, MSG again) never loses a
     * recording or leaves the radio keyed. */
    if (state == MSG_VOICE_RECORD) stop_rec();
    reap_worker(true);
    adjusting = false;
    if (renaming) {
        textarea_window_close();
        renaming = false;
    }
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    lv_group_remove_obj(dialog.obj);
    delete_armed = 0;
}
