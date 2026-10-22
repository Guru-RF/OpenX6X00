/*
 * SSTV app: window, buttons and the receivers.
 *
 *  Auto RX   listens for a VIS header (1-s window every 50 ms), keeps the
 *            2 s before it, records the picture for its known length (a new
 *            header restarts the recording if Restart is on), then decodes
 *            it and listens again.
 *  Manual RX records with no header: the mode set on the RX Mode button, or
 *            Auto = recognised from the line syncs; the sync follower also
 *            ends the recording when the picture is over.
 * Rows are drawn while they arrive; the decoded picture replaces them and
 * is saved in /mnt/sstv as sstv-YYYYmmdd-HHMMSS.png (+ _dn.png cleaned).
 *
 * Audio comes from audio_cb() in the audio thread; it only copies and sets
 * flags under rx_lock.  LVGL is touched only from the 30-ms UI timer and
 * the button callbacks.
 */
#include "app_sstv.h"
#include "board.h"
#include "app_sstv_int.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "events.h"
#include "freq_list.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "params/params.h"
#include "radio.h"
#include "sstv/denoise.h"
#include "sstv/sstv_image.h"
#include "sstv/sstv_png.h"
#include "sstv/sstv_sync.h"
#include "sstv/sstv_vis.h"
#include "styles.h"
#include "theme.h"
#include "tx_player.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_PWR      TX_PLAYER_MAX_PWR_W

#define RING_N       (SSTV_RATE * 2)          /* audio kept before a header */
#define VIS_N        SSTV_RATE                /* header window */
#define VIS_EVERY    (SSTV_RATE / 20)         /* 50 ms */
#define VIS_HOLD     (SSTV_RATE * 3 / 2)      /* "not supported" waits this long for a good header */
#define MANUAL_MAX_S 300
#define ROWS_EVERY   200                      /* ms between drawings of new rows */
#define ROWS_AT_ONCE 8
#define ROWS_AUDIO   (2 * 1051 * SSTV_RATE / 1000 + 256)   /* two Scottie DX lines */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void audio_cb(unsigned int n, float *samples);
static void timer_cb(lv_timer_t *t);
static void auto_cb(button_data_t *b);
static void manual_cb(button_data_t *b);
static void mode_cb(button_data_t *b);
static void tx_cb(button_data_t *b);
static void freq_cb(button_data_t *b);
static void gallery_cb(button_data_t *b);
static void delete_cb(button_data_t *b);
static void restart_cb(button_data_t *b);
static void denoise_cb(button_data_t *b);
static void close_cb(button_data_t *b);
static void page_cb(button_data_t *b);

/* ---- Window and buttons ---- */

static button_data_t btn_p1 = { .type = BTN_TEXT, .label = "(SSTV 1:3)", .press = page_cb };
static button_data_t btn_p2 = { .type = BTN_TEXT, .label = "(SSTV 2:3)", .press = page_cb };
static button_data_t btn_p3 = { .type = BTN_TEXT, .label = "(SSTV 3:3)", .press = page_cb };
static button_data_t btn_auto = { .type = BTN_TEXT, .label = "Start\nAuto RX", .press = auto_cb };
static button_data_t btn_manual = { .type = BTN_TEXT, .label = "Start\nManual RX", .press = manual_cb };
button_data_t sstv_btn_mode = { .type = BTN_TEXT, .label = "RX Mode\nAuto", .press = mode_cb };
button_data_t sstv_btn_tx = { .type = BTN_TEXT, .label = "TX", .press = tx_cb };
static button_data_t btn_freq = { .type = BTN_TEXT, .label = "Freq.", .press = freq_cb };
button_data_t sstv_btn_gallery = { .type = BTN_TEXT, .label = "RX Pics\nBrowser", .press = gallery_cb };
button_data_t sstv_btn_delete = { .type = BTN_TEXT, .label = "Delete\npicture", .press = delete_cb };
static button_data_t btn_restart = { .type = BTN_TEXT, .label = "Auto RX\nRestart ON", .press = restart_cb };
static button_data_t btn_denoise = { .type = BTN_TEXT, .label = "Denoise\nON", .press = denoise_cb };
static button_data_t btn_close = { .type = BTN_TEXT, .label = "Close", .press = close_cb };

static buttons_page_t page1 = { .items = { &btn_p1, &btn_auto, &btn_manual, &sstv_btn_mode, &sstv_btn_tx } };
static buttons_page_t page2 = { .items = { &btn_p2, &btn_freq, &sstv_btn_gallery, &sstv_btn_delete, &btn_restart } };
static buttons_page_t page3 = { .items = { &btn_p3, NULL, &btn_dig_pwr, &btn_denoise, &btn_close } };

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .audio_cb = audio_cb,
    .key_cb = sstv_key_cb,
    .restore_radio = true,
    .btn_page = &page1,
};

dialog_t *dialog_sstv = &dialog;

app_frame_t  sstv_frame;
lv_obj_t    *sstv_canvas, *sstv_text, *sstv_bar;
lv_color_t   sstv_pixels[SSTV_PIC_PIXELS];
sstv_mode_t  sstv_mode = SSTV_ROBOT8;
bool         sstv_mode_auto = true;

static char         mode_label[48];
static lv_timer_t  *timer;

/* SSTV calling frequencies; LSB below 10 MHz as usual on the air */
static const freq_list_item_t freqs[] = {
    { .group = "LSB" },
    { .freq_hz = 3735000, .name = "SSTV 80m", .note = "Europe", .lsb = true },
    { .freq_hz = 3845000, .name = "SSTV 80m", .note = "Americas", .lsb = true },
    { .freq_hz = 7165000, .name = "SSTV 40m", .note = "Europe", .lsb = true },
    { .freq_hz = 7171000, .name = "SSTV 40m", .note = "Americas", .lsb = true },
    { .group = "USB" },
    { .freq_hz = 14230000, .name = "SSTV 20m", .note = "calling" },
    { .freq_hz = 14233000, .name = "SSTV 20m" },
    { .freq_hz = 18160000, .name = "SSTV 17m" },
    { .freq_hz = 21340000, .name = "SSTV 15m" },
    { .freq_hz = 24975000, .name = "SSTV 12m" },
    { .freq_hz = 28680000, .name = "SSTV 10m" },
    { .freq_hz = 50510000, .name = "SSTV 6m" },
};
#define NFREQS (int)(sizeof(freqs) / sizeof(freqs[0]))

/* ---- Receiver state ---- */

static pthread_mutex_t rx_lock = PTHREAD_MUTEX_INITIALIZER;

/* the picture shown and saved, filled by the decoder thread */
static uint8_t       pic[SSTV_MAX_W * SSTV_MAX_H * 3];
static uint8_t       pic_tmp[SSTV_MAX_W * SSTV_MAX_H * 3];     /* sstv_denoise() scratch */
static sstv_mode_t   pic_mode;
static pthread_t     worker;
static bool          worker_on;               /* worker to join */
static volatile bool working;                 /* worker running */
static volatile bool pic_ready;
static volatile int  pic_rows;
static int           pic_shown;
static volatile bool saved_note;
static char          saved_name[96];

/* Auto RX */
static float          ring[RING_N], vis_win[VIS_N];
static size_t         ring_at, ring_len;
static unsigned       since_vis;
static uint64_t       heard, heard_note;      /* audio listened to, for the log */
static volatile bool  auto_on;                /* listening (or recording) */
static volatile bool  auto_rec;               /* recording a picture */
static volatile bool  auto_ready;             /* recorded, to decode */
static volatile bool  auto_dec;               /* decoding */
static volatile bool  auto_drop;              /* Halt while decoding: throw the result away */
static volatile bool  auto_clear;             /* clear the picture area */
static volatile bool  auto_label;             /* mode button to update */
static volatile bool  restart = true;         /* a new header restarts the recording */
static volatile int   auto_unsupported = -1;  /* code to report */
static int            held_code = -1;         /* unsupported code waiting for VIS_HOLD */
static uint64_t       vis_clock, held_at;      /* samples looked at for headers */
static float         *rec;
static long           rec_n, rec_want;
static sstv_mode_t    rec_mode;
static float          vis_at = -1.0f;         /* header start in the last window */

/* Manual RX */
static volatile bool  man_rec, man_ready, man_dec, man_locked, man_lock_note;
static float         *man;
static long           man_n, man_room, man_limit;
static sstv_mode_t    man_mode;               /* to decode with; SSTV_UNKNOWN: classify */
static sstv_sync_t    man_sync;
static const char *volatile man_fail;

/* rows while receiving */
static sstv_sync_t    auto_sync;
static sstv_rows_t    auto_rows, man_rows;
static volatile bool  auto_rows_on, man_rows_on;
static float          rows_audio[ROWS_AUDIO];

static const char *mode_label_of(sstv_mode_t m)
{
    const sstv_mode_info_t *mi = sstv_mode_info(m);
    return mi ? mi->label : "Unknown";
}

static const char *rx_mode_name(void)
{
    return sstv_mode_auto ? "Auto" : mode_label_of(sstv_mode);
}

/* The mode a Manual recording is decoded with (rx_lock held) */
static sstv_mode_t man_decode_mode(void)
{
    if (!sstv_mode_auto) return sstv_mode;
    return man_locked ? man_sync.st.mode : SSTV_UNKNOWN;
}

static void ring_reset(void)
{
    ring_at = ring_len = 0;
    since_vis = 0;
    heard = heard_note = 0;
    held_code = -1;
}

static void rec_free(void)
{
    free(rec);
    rec = NULL;
    rec_n = rec_want = 0;
}

static void man_free(void)
{
    free(man);
    man = NULL;
    man_n = man_room = 0;
}

/* ---- Small UI helpers ---- */

/* VISION: under the state line only what is news, one line apart: not the first line (the state
 * line's) nor the instructions ("Press ...", the buttons say them) */
static void vision_details(char *out, size_t size, const char *text)
{
    const char *p = strchr(text, '\n');
    size_t      o = 0;

    out[0] = 0;
    while (p && *p) {
        size_t n;

        p += strspn(p, "\n");
        n = strcspn(p, "\n");
        if (n && strncmp(p, "Press ", 6) && strncmp(p, "- Press ", 8) && o + n + 2 < size)
            o += snprintf(out + o, size - o, "%s%.*s", o ? "\n" : "", (int)n, p);
        p += n;
    }
}

void sstv_status(const char *text)
{
    char        vis[256];
    const char *body = text;

    if (theme_shape.vision && text) {
        vision_details(vis, sizeof(vis), text);
        body = vis;
    }
    if (sstv_text) lv_label_set_text(sstv_text, body ? body : "");
    if (sstv_frame.head_right && text) {      /* first line, no full stop, in the header */
        char   head[48];
        size_t n = strcspn(text, "\n");
        snprintf(head, sizeof(head), "%.*s", (int)n, text);
        n = strlen(head);
        if (n && head[n - 1] == '.') head[n - 1] = 0;
        lv_label_set_text(sstv_frame.head_right, head);
    }
}

void sstv_mode_button(bool tx, sstv_mode_t tx_mode)
{
    snprintf(mode_label, sizeof(mode_label), "%s Mode\n%s", tx ? "TX" : "RX",
             tx ? mode_label_of(tx_mode) : rx_mode_name());
    sstv_btn_mode.label = mode_label;
    if (sstv_btn_mode.disp_btn) buttons_refresh(&sstv_btn_mode);
}

void sstv_focus_picture(void)
{
    lv_group_add_obj(keyboard_group, sstv_canvas);
    lv_group_focus_obj(sstv_canvas);
    lv_group_set_editing(keyboard_group, true);
}

void sstv_key_cb(lv_event_t *e)
{
    switch (*((uint32_t *)lv_event_get_param(e))) {
    case KEY_VOL_LEFT_EDIT:
    case KEY_VOL_LEFT_SELECT:
        radio_change_vol(-1);
        break;
    case KEY_VOL_RIGHT_EDIT:
    case KEY_VOL_RIGHT_SELECT:
        radio_change_vol(1);
        break;
    default:
        break;
    }
}

void sstv_controls(void)
{
    bool a = auto_on || auto_rec || auto_ready || auto_dec;
    bool m = man_rec || man_ready || man_dec;

    if (sstv_tx_state != SSTV_TX_OFF) {
        buttons_disabled(&btn_auto, true);
        buttons_disabled(&btn_manual, true);
        buttons_disabled(&sstv_btn_mode, sstv_tx_state == SSTV_TX_ON_AIR);
        page1.items[3] = &sstv_btn_mode;
    } else {
        buttons_disabled(&btn_auto, m);
        buttons_disabled(&btn_manual, a || man_ready || man_dec);
        buttons_disabled(&sstv_btn_mode, a || m);
        page1.items[3] = !a && !m ? &sstv_btn_mode : NULL;    /* only while both are idle */
    }
    if (buttons_get_cur_page() == &page1) buttons_load_page(&page1);
}

static void bar_hide(void)
{
    if (!sstv_bar) return;
    lv_bar_set_value(sstv_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(sstv_bar, LV_OBJ_FLAG_HIDDEN);
}

static void auto_button(const char *label)
{
    btn_auto.label = label;
    buttons_disabled(&btn_auto, false);
    if (btn_auto.disp_btn) buttons_refresh(&btn_auto);
    sstv_controls();
}

static void show_standby(void)
{
    sstv_status("Stand by.\n\n- Press Start Auto RX to start Autodecoder.\n\n- Press Start Manual RX to start Manual "
                "decoder, press RX Mode button to cycle Auto (mode from the line syncs) and all available modes for "
                "the Manual decoder.");
    bar_hide();
    auto_button("Start\nAuto RX");
}

static void show_listening(void)
{
    sstv_status("SSTV Autodecoder is running.\n\nAwaiting signal...");
    bar_hide();
    auto_button("Stop\nAuto RX");
}

static void show_decoding(const char *name)
{
    char s[96];
    snprintf(s, sizeof(s), "Decoding %s", name ? name : "SSTV");
    sstv_status(s);
    if (sstv_bar) lv_obj_clear_flag(sstv_bar, LV_OBJ_FLAG_HIDDEN);
    auto_button("Halt\nDecoding");
}

static void header(void)
{
    char l[64];
    snprintf(l, sizeof(l), "SSTV   %.3f MHz", cparam_i_get(cfg_fg_freq) / 1e6);
    if (strcmp(l, lv_label_get_text(sstv_frame.head_left))) lv_label_set_text(sstv_frame.head_left, l);
}

/* w x h RGB stretched into the picture area (aspect kept, centred) */
static void fit(int w, int h, int *ow, int *oh, int *x0, int *y0)
{
    *oh = SSTV_PIC_H;
    *ow = w * *oh / h;
    if (*ow > SSTV_PIC_W) {
        *ow = SSTV_PIC_W;
        *oh = h * *ow / w;
    }
    *x0 = (SSTV_PIC_W - *ow) / 2;
    *y0 = (SSTV_PIC_H - *oh) / 2;
}

static void show_picture(void)
{
    const sstv_mode_info_t *mi = sstv_mode_info(pic_mode);
    int                     ow, oh, x0, y0;

    if (!sstv_canvas || pic_rows <= 0 || !mi) return;
    lv_canvas_fill_bg(sstv_canvas, lv_color_white(), LV_OPA_COVER);
    fit(mi->width, mi->height, &ow, &oh, &x0, &y0);
    for (int y = 0; y < oh; ++y) {
        int sy = (int)((int64_t)y * mi->height / oh);
        for (int x = 0; x < ow; ++x) {
            const uint8_t *p = pic + ((size_t)sy * mi->width + (int)((int64_t)x * mi->width / ow)) * 3;
            lv_canvas_set_px(sstv_canvas, x0 + x, y0 + y, lv_color_make(p[0], p[1], p[2]));
        }
    }
    lv_obj_invalidate(sstv_canvas);
}

/* ---- Saving ---- */

static bool save_png(const uint8_t *rgb, const sstv_mode_info_t *mi, const char *name)
{
    char path[160];

    if (mkdir(SSTV_DIR, 0775) != 0 && errno != EEXIST) {
        fprintf(stderr, "SSTV: cannot make %s: %s\n", SSTV_DIR, strerror(errno));
        return false;
    }
    snprintf(path, sizeof(path), SSTV_DIR "/%s", name);
    if (!sstv_png_write(path, rgb, mi->width, mi->height)) {
        fprintf(stderr, "SSTV: cannot write %s\n", path);
        return false;
    }
    fprintf(stderr, "SSTV: saved %s\n", path);
    return true;
}

/* Decoder threads: pic saved as sstv-STAMP.png; with Denoise on, pic is
 * cleaned (and shown so) and saved as sstv-STAMP_dn.png too */
static void save_picture(void)
{
    const sstv_mode_info_t *mi = sstv_mode_info(pic_mode);
    char                    stem[40], name[48];
    time_t                  now = time(NULL);
    struct tm               tm;

    if (!mi || !localtime_r(&now, &tm)) return;
    strftime(stem, sizeof(stem), "sstv-%Y%m%d-%H%M%S", &tm);
    snprintf(name, sizeof(name), "%s.png", stem);
    bool ok = save_png(pic, mi, name);
    if (params.sstv_denoise.x) {
        sstv_denoise(pic, pic_tmp, mi->width, mi->height);
        snprintf(name, sizeof(name), "%s_dn.png", stem);
        ok = save_png(pic, mi, name) || ok;
    }
    if (ok) {
        snprintf(saved_name, sizeof(saved_name), "%s", name);
        saved_note = true;
    }
}

/* ---- Decoder threads ---- */

static void *auto_thread(void *arg)
{
    (void)arg;
    pic_mode = rec_mode;
    int  rows = sstv_decode(rec, rec_n, rec_mode, pic);
    bool dropped = auto_drop;
    pic_rows = !dropped && rows > 0 ? rows : 0;
    if (!dropped && rows > 0) save_picture();
    pic_ready = !dropped;
    auto_drop = false;
    rec_free();
    working = false;
    auto_dec = false;
    pthread_mutex_lock(&rx_lock);
    ring_reset();
    auto_on = true;
    pthread_mutex_unlock(&rx_lock);
    fprintf(stderr, "SSTV: Auto RX listening again (%d rows)\n", rows);
    return NULL;
}

static void *manual_thread(void *arg)
{
    (void)arg;
    if (man_mode == SSTV_UNKNOWN)     /* Auto with no lock: the mode of the whole recording */
        man_mode = sstv_sync_classify(man, man_n, SSTV_UNKNOWN, NULL);
    pic_mode = man_mode;
    int rows = man_mode == SSTV_UNKNOWN ? -1 : sstv_decode_manual(man, man_n, man_mode, pic);
    pic_rows = rows > 0 ? rows : 0;
    if (rows > 0) save_picture();
    pic_ready = true;
    man_free();
    working = false;
    man_dec = false;
    if (rows <= 0) man_fail = man_mode == SSTV_UNKNOWN ? "No SSTV picture found" : "Decoding failed";
    fprintf(stderr, "SSTV: Manual RX done (%d rows)\n", rows);
    return NULL;
}

static bool start_worker(void *(*fn)(void *))
{
    pic_shown = 0;
    pic_rows = 0;
    pic_ready = false;
    working = true;
    int rc = pthread_create(&worker, NULL, fn, NULL);
    if (rc) {
        fprintf(stderr, "SSTV: cannot start the decoder: %d\n", rc);
        working = false;
        return false;
    }
    worker_on = true;
    return true;
}

/* ---- Rows while receiving ---- */

/* Auto RX recording rec[0..rec_n) just started from the header found in its
 * last VIS_N samples (rx_lock held) */
static void auto_rows_start(void)
{
    double v = (double)(rec_n - VIS_N) + vis_at * SSTV_RATE;
    double row0 = v + (SSTV_VIS_S + sstv_row0_after_vis(rec_mode)) * SSTV_RATE;

    sstv_sync_init(&auto_sync, rec_mode);
    sstv_sync_feed(&auto_sync, rec, (size_t)rec_n);
    sstv_rows_init(&auto_rows, rec_mode, v >= 0.0 ? row0 : -1.0);
    auto_rows_on = true;
}

/* Draws up to ROWS_AT_ONCE rows whose audio is in buf, scaled like
 * show_picture() straight into the canvas buffer.  The audio and the row
 * state are copied under rx_lock (audio_cb() may restart the recording),
 * the row is drawn outside it.  false: the recording is over. */
static bool rows_tick(sstv_rows_t *r, const sstv_sync_t *s, float *const *buf, const long *n,
                      volatile bool *recording)
{
    uint8_t     line[SSTV_MAX_W * 3];
    sstv_rows_t one;
    int         drawn = 0;

    for (int k = 0; k < ROWS_AT_ONCE; ++k) {
        long a, b;
        bool go = false, on = true;
        pthread_mutex_lock(&rx_lock);
        if (!*recording || !*buf)
            on = false;
        else if (sstv_rows_ready(r, *n, &s->st, &a, &b) && b - a <= ROWS_AUDIO) {
            memcpy(rows_audio, *buf + a, (size_t)(b - a) * sizeof(float));
            one = *r;
            r->next++;
            go = true;
        }
        pthread_mutex_unlock(&rx_lock);
        if (!on) return false;
        if (!go) break;
        int row = sstv_rows_draw(&one, rows_audio, line);
        if (row < 0 || !sstv_canvas) continue;

        int w = one.width, h = one.height, ow, oh, x0, y0;
        fit(w, h, &ow, &oh, &x0, &y0);
        int y1 = (int)(((int64_t)(row + 1) * oh + h - 1) / h);
        for (int y = (int)(((int64_t)row * oh + h - 1) / h); y < y1 && y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                const uint8_t *p = line + (size_t)(x * w / ow) * 3;
                sstv_pixels[(size_t)(y0 + y) * SSTV_PIC_W + x0 + x] = lv_color_make(p[0], p[1], p[2]);
            }
        ++drawn;
    }
    if (drawn && sstv_canvas) lv_obj_invalidate(sstv_canvas);
    return true;
}

/* ---- Audio thread ---- */

/* VIS in the last second of the ring (rx_lock held) */
static bool vis_in_ring(sstv_mode_t *mode)
{
    size_t     from = (ring_at + RING_N - VIS_N) % RING_N;
    sstv_vis_t v;

    for (size_t i = 0; i < VIS_N; ++i) vis_win[i] = ring[(from + i) % RING_N];
    if (sstv_vis_find(vis_win, VIS_N, &v)) {
        held_code = -1;
        vis_at = v.start_s + v.gap_s;
        *mode = v.mode;
        fprintf(stderr, "SSTV: VIS %s, %+.1f Hz, score %.2f\n", sstv_mode_info(v.mode)->name, v.offset_hz, v.score);
        return true;
    }
    /* A header of a mode we do not decode is said only if no good header
     * comes within VIS_HOLD: the leader of a real picture can first read as
     * VIS 0 or 15 while its bits are still arriving */
    if (v.unsupported >= 0 && held_code < 0) {
        held_code = v.unsupported;
        held_at = vis_clock;
    }
    if (held_code >= 0 && vis_clock - held_at >= VIS_HOLD) {
        auto_unsupported = held_code;
        held_code = -1;
    }
    return false;
}

static void ring_add(const float *x, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        ring[ring_at] = x[i];
        ring_at = (ring_at + 1) % RING_N;
        if (ring_len < RING_N) ++ring_len;
    }
    since_vis += n;
    vis_clock += n;
}

/* A new recording of mode starting with the ring (rx_lock held).  false: no memory */
static bool rec_start(sstv_mode_t mode)
{
    long   want = (long)((sstv_mode_info(mode)->capture_s + 2.0f) * SSTV_RATE);
    float *buf = malloc((size_t)want * sizeof(float));

    if (!buf) {
        fprintf(stderr, "SSTV: no memory for %.1f MB of recording\n", (double)want * sizeof(float) / 1048576.0);
        return false;
    }
    size_t keep = ring_len < (size_t)want ? ring_len : (size_t)want;
    size_t old = (ring_at + RING_N - ring_len) % RING_N;
    for (size_t i = 0; i < keep; ++i) buf[i] = ring[(old + i) % RING_N];
    free(rec);
    rec = buf;
    rec_n = (long)keep;
    rec_want = want;
    rec_mode = mode;
    auto_clear = true;
    auto_rows_start();
    if (mode != SSTV_SCOTTIE_DX) {      /* ready to answer in the same mode (we do not send DX) */
        sstv_mode = mode;
        auto_label = true;
    }
    fprintf(stderr, "SSTV: recording %s, %.1f s\n", sstv_mode_info(mode)->name, (double)want / SSTV_RATE);
    return true;
}

static void manual_audio(const float *x, unsigned n)
{
    long room = man_room - man_n, k = (long)n < room ? (long)n : room;

    if (k > 0) {
        memcpy(man + man_n, x, (size_t)k * sizeof(float));
        man_n += k;
        sstv_sync_feed(&man_sync, x, (size_t)k);
    }
    const sstv_sync_status_t *st = &man_sync.st;
    if (!man_locked && st->state != SSTV_SYNC_SEARCH) {
        man_locked = true;
        man_lock_note = true;
        if (sstv_mode_auto) {             /* the whole picture from its first sync, then stop */
            long end = (long)st->first_sync + (long)((sstv_sync_picture_s(st->mode) + 5.0) * SSTV_RATE);
            if (end < man_limit) man_limit = end;
            sstv_rows_init(&man_rows, st->mode, -1.0);
            man_rows_on = true;
        }
        fprintf(stderr, "SSTV: Manual RX, %s line syncs after %.1f s\n", mode_label_of(st->mode),
                (double)man_n / SSTV_RATE);
    }
    if (man_n >= man_limit || st->state == SSTV_SYNC_LOST) {
        man_rec = false;
        man_mode = man_decode_mode();
        man_ready = true;
        fprintf(stderr, "SSTV: Manual RX, %s after %.1f s (%+.0f ppm), decoding %s\n",
                st->state == SSTV_SYNC_LOST ? "picture over" : "time limit", (double)man_n / SSTV_RATE,
                st->clock_ppm, mode_label_of(man_mode));
    }
}

static void audio_cb(unsigned int n, float *x)
{
    sstv_mode_t mode;

    if ((!auto_on && !man_rec) || !x || !n || working) return;
    pthread_mutex_lock(&rx_lock);
    if (man_rec) {
        manual_audio(x, n);
    } else if (auto_on && auto_rec) {
        long k = rec_want - rec_n;
        if ((long)n < k) k = n;
        if (k > 0) {
            memcpy(rec + rec_n, x, (size_t)k * sizeof(float));
            rec_n += k;
            sstv_sync_feed(&auto_sync, x, (size_t)k);
        }
        if (rec_n >= rec_want) {
            auto_rec = false;
            auto_ready = true;
            auto_on = false;
            fprintf(stderr, "SSTV: %s recorded\n", sstv_mode_info(rec_mode)->name);
        } else if (restart) {
            /* a new header while recording starts over with it; the ring was
             * emptied at the start, so the header that began this picture
             * cannot be found again */
            ring_add(x, n);
            if (ring_len >= VIS_N && since_vis >= VIS_EVERY) {
                since_vis = 0;
                if (vis_in_ring(&mode)) {
                    fprintf(stderr, "SSTV: new VIS, %s recording dropped at %.1f s\n",
                            sstv_mode_info(rec_mode)->name, (double)rec_n / SSTV_RATE);
                    if (rec_start(mode)) ring_reset();
                }
            }
        }
    } else if (auto_on) {
        ring_add(x, n);
        heard += n;
        if (heard - heard_note >= (uint64_t)SSTV_RATE * 60u) {
            heard_note = heard;
            fprintf(stderr, "SSTV: Auto RX, %llu s listened, no VIS yet\n", (unsigned long long)(heard / SSTV_RATE));
        }
        if (ring_len >= VIS_N && since_vis >= VIS_EVERY) {
            since_vis = 0;
            if (vis_in_ring(&mode)) {
                if (!rec_start(mode)) {
                    auto_on = false;
                } else {
                    auto_rec = true;
                    if (restart) ring_reset();   /* look for the next header in new audio only */
                }
            }
        }
    }
    pthread_mutex_unlock(&rx_lock);
}

/* ---- UI timer ---- */

static void timer_cb(lv_timer_t *t)
{
    static uint32_t rows_at;
    (void)t;

    if (sstv_frame.head_left) header();
    sstv_tx_tick();

    if (auto_clear) {
        auto_clear = false;
        if (sstv_canvas) {
            lv_canvas_fill_bg(sstv_canvas, lv_color_white(), LV_OPA_COVER);
            lv_obj_invalidate(sstv_canvas);
        }
    }
    if (auto_label) {
        auto_label = false;
        sstv_mode_button(false, sstv_mode);
    }
    if (auto_unsupported >= 0) {
        msg_schedule_text_fmt("SSTV mode not supported (VIS %d)", auto_unsupported);
        auto_unsupported = -1;
    }
    if (man_lock_note) {
        char s[192];
        man_lock_note = false;
        snprintf(s, sizeof(s),
                 "SSTV Manual decoder is running\n\n%s line syncs found\n\nPress Stop Manual RX to render received picture",
                 mode_label_of(man_sync.st.mode));
        if (man_rec) sstv_status(s);
    }
    if ((auto_rows_on || man_rows_on) && lv_tick_elaps(rows_at) >= ROWS_EVERY) {
        rows_at = lv_tick_get();
        if (auto_rows_on && !rows_tick(&auto_rows, &auto_sync, &rec, &rec_n, &auto_rec)) auto_rows_on = false;
        if (man_rows_on && !rows_tick(&man_rows, &man_sync, &man, &man_n, &man_rec)) man_rows_on = false;
    }
    if (man_fail) {
        msg_schedule_text_fmt("%s", man_fail);
        man_fail = NULL;
        pic_ready = false;
        show_standby();
    }
    if (saved_note) {
        const sstv_mode_info_t *mi = sstv_mode_info(pic_mode);
        char                    s[160];
        saved_note = false;
        snprintf(s, sizeof(s), "SSTV picture %s saved", saved_name);
        msg_schedule_text_fmt("%s", s);
    }
    sstv_tx_tick();

    if (worker_on && !working) {
        pthread_join(worker, NULL);
        worker_on = false;
    }
    if (man_ready && !working && !worker_on) {
        char s[96];
        man_ready = false;
        man_dec = true;
        snprintf(s, sizeof(s), "Decoding Cont %s", man_mode == SSTV_UNKNOWN ? "Auto" : mode_label_of(man_mode));
        sstv_status(s);
        if (sstv_bar) lv_obj_add_flag(sstv_bar, LV_OBJ_FLAG_HIDDEN);
        sstv_controls();
        if (!start_worker(manual_thread)) man_dec = false;
    }
    if (auto_ready && !working && !worker_on) {
        auto_ready = false;
        auto_dec = true;
        show_decoding(sstv_mode_info(rec_mode)->name);
        if (sstv_bar) lv_bar_set_value(sstv_bar, 1000, LV_ANIM_OFF);
        if (!start_worker(auto_thread)) auto_dec = false;
    }
    if (auto_rec && rec_want > 0) {       /* the bar follows the recording */
        show_decoding(sstv_mode_info(rec_mode)->name);
        if (sstv_bar) {
            long v = rec_n * 1000L / rec_want;
            lv_bar_set_value(sstv_bar, v < 0 ? 0 : v > 1000 ? 1000 : (int)v, LV_ANIM_OFF);
        }
    }
    /* an Auto decode that gave nothing: say so and listen again */
    if (pic_ready && pic_rows <= 0 && auto_on) {
        pic_ready = false;
        pic_shown = 0;
        if (sstv_bar) lv_bar_set_value(sstv_bar, 0, LV_ANIM_OFF);
        msg_schedule_text_fmt("Decoding failed");
        show_listening();
        return;
    }
    if (!sstv_canvas || !pic_ready || pic_shown >= pic_rows) return;
    show_picture();
    pic_shown = pic_rows;
    if (sstv_bar) lv_bar_set_value(sstv_bar, 1000, LV_ANIM_OFF);
    if (auto_on)
        show_listening();
    else
        show_standby();
}

/* ---- Buttons ---- */

/* (SSTV 1:3) -> 2 -> 3 -> 1 */
static void page_cb(button_data_t *b)
{
    buttons_load_page(b == &btn_p1 ? &page2 : b == &btn_p2 ? &page3 : &page1);
    if (b == &btn_p1) sstv_gallery_labels();
}

static void freq_cb(button_data_t *b)
{
    (void)b;
    if (freq_list_is_open())
        freq_list_close();
    else
        freq_list_open(dialog.obj, "SSTV - calling frequencies", freqs, NFREQS, 0, NULL);
}

static void gallery_cb(button_data_t *b)
{
    (void)b;
    sstv_gallery_button();
}

static void delete_cb(button_data_t *b)
{
    (void)b;
    sstv_gallery_second();
}

static void tx_cb(button_data_t *b)
{
    (void)b;
    sstv_tx_button();
}

static void restart_cb(button_data_t *b)
{
    (void)b;
    pthread_mutex_lock(&rx_lock);
    restart = !restart;
    /* switched on while recording: look for headers in new audio only, not
     * the one that started this picture */
    if (restart && auto_rec) {
        ring_at = ring_len = 0;
        since_vis = 0;
    }
    pthread_mutex_unlock(&rx_lock);
    btn_restart.label = restart ? "Auto RX\nRestart ON" : "Auto RX\nRestart OFF";
    if (btn_restart.disp_btn) buttons_refresh(&btn_restart);
}

/* From the next picture on; the picture as received is always saved too */
static void denoise_cb(button_data_t *b)
{
    (void)b;
    params_bool_set(&params.sstv_denoise, !params.sstv_denoise.x);
    btn_denoise.label = params.sstv_denoise.x ? "Denoise\nON" : "Denoise\nOFF";
    if (btn_denoise.disp_btn) buttons_refresh(&btn_denoise);
}

static void close_cb(button_data_t *b)
{
    (void)b;
    dialog_destruct();
}

static void mode_cb(button_data_t *b)
{
    (void)b;
    if (sstv_tx_state == SSTV_TX_ON_AIR) return;
    if (sstv_tx_state == SSTV_TX_OFF && (auto_on || auto_rec || man_rec || man_ready || working)) return;

    int i = 0, n = (int)(sizeof(sstv_mode_cycle) / sizeof(sstv_mode_cycle[0]));
    while (i < n && sstv_mode_cycle[i] != sstv_mode) ++i;
    if (sstv_tx_state == SSTV_TX_OFF && sstv_mode_auto) {        /* RX: Auto, every mode, Auto */
        sstv_mode_auto = false;
        sstv_mode = sstv_mode_cycle[0];
    } else if (sstv_tx_state == SSTV_TX_OFF && i + 1 >= n) {
        sstv_mode_auto = true;
    } else {
        sstv_mode = sstv_mode_cycle[(i + 1) % n];
    }
    sstv_mode_button(sstv_tx_state != SSTV_TX_OFF, sstv_mode);
    if (sstv_tx_state == SSTV_TX_SETUP) sstv_tx_mode_changed();
}

static void manual_cb(button_data_t *b)
{
    char s[192];
    (void)b;

    pthread_mutex_lock(&rx_lock);
    if (man_rec) {                        /* Stop: decode what came in */
        man_rec = false;
        if (man_n > 0) {
            man_mode = man_decode_mode();
            man_ready = true;
            fprintf(stderr, "SSTV: Manual RX stopped after %.1f s, decoding %s\n", (double)man_n / SSTV_RATE,
                    mode_label_of(man_mode));
        } else {
            man_free();
        }
        pthread_mutex_unlock(&rx_lock);
        btn_manual.label = "Start\nManual RX";
        if (btn_manual.disp_btn) buttons_refresh(&btn_manual);
        sstv_controls();
        return;
    }
    if (auto_on || auto_rec || auto_ready || working) {
        pthread_mutex_unlock(&rx_lock);
        return;
    }

    /* Auto: the limit becomes the picture length once the mode is known */
    float limit = sstv_mode_auto ? (float)MANUAL_MAX_S : sstv_mode_info(sstv_mode)->capture_s + 5.0f;
    if (limit > (float)MANUAL_MAX_S) limit = (float)MANUAL_MAX_S;
    man_room = (long)(limit * (float)SSTV_RATE);
    man = malloc((size_t)man_room * sizeof(float));
    if (!man) {
        man_room = 0;
        pthread_mutex_unlock(&rx_lock);
        fprintf(stderr, "SSTV: no memory for Manual RX\n");
        return;
    }
    man_n = 0;
    man_limit = man_room;
    man_locked = false;
    sstv_sync_init(&man_sync, sstv_mode_auto ? SSTV_UNKNOWN : sstv_mode);
    /* a set mode: rows from the first sync the follower finds; Auto: from the lock */
    if (!sstv_mode_auto) sstv_rows_init(&man_rows, sstv_mode, -1.0);
    man_rows_on = !sstv_mode_auto;
    man_rec = true;
    pthread_mutex_unlock(&rx_lock);

    if (sstv_canvas) lv_canvas_fill_bg(sstv_canvas, lv_color_white(), LV_OPA_COVER);
    if (sstv_mode_auto)
        snprintf(s, sizeof(s), "SSTV Manual decoder is running\n\nSearching the line syncs for the SSTV mode\n\n"
                               "Press Stop Manual RX to render received picture");
    else
        snprintf(s, sizeof(s),
                 "SSTV Manual decoder is running\n\nDecoding %s signal\n\nPress Stop Manual RX to render received picture",
                 mode_label_of(sstv_mode));
    sstv_status(s);
    if (sstv_bar) lv_obj_add_flag(sstv_bar, LV_OBJ_FLAG_HIDDEN);
    btn_manual.label = "Stop\nManual RX";
    if (btn_manual.disp_btn) buttons_refresh(&btn_manual);
    sstv_controls();
    fprintf(stderr, "SSTV: Manual RX %s, stops after %.0f s at most\n", rx_mode_name(), limit);
}

static void auto_cb(button_data_t *b)
{
    (void)b;
    pthread_mutex_lock(&rx_lock);
    if (man_rec || man_ready || man_dec) {
        pthread_mutex_unlock(&rx_lock);
        return;
    }
    if (auto_rec || auto_ready || auto_dec) {
        /* Halt Decoding: this picture is dropped, Auto RX goes on */
        auto_drop = true;
        auto_rec = auto_ready = false;
        ring_reset();
        if (!auto_dec) {
            rec_free();
            auto_drop = false;
        }
        auto_on = true;
        pthread_mutex_unlock(&rx_lock);
        show_listening();
        return;
    }
    if (auto_on) {                        /* Stop */
        auto_on = false;
        ring_reset();
        rec_free();
        pthread_mutex_unlock(&rx_lock);
        show_standby();
        return;
    }
    if (!working) {
        auto_on = true;
        auto_drop = false;
        ring_reset();
        rec_free();
        fprintf(stderr, "SSTV: Auto RX listening\n");
    }
    pthread_mutex_unlock(&rx_lock);
    show_listening();
}

/* ---- With TX ---- */

bool sstv_rx_stop_for_tx(void)
{
    bool was = auto_on || auto_rec || auto_ready || auto_dec;

    pthread_mutex_lock(&rx_lock);
    auto_on = auto_rec = auto_ready = false;
    ring_reset();
    rec_free();
    man_rec = man_ready = false;    /* a Manual recording is dropped: TX comes first */
    man_free();
    pthread_mutex_unlock(&rx_lock);
    return was;
}

void sstv_rx_back(bool auto_rx)
{
    if (auto_rx) {
        pthread_mutex_lock(&rx_lock);
        auto_on = true;
        auto_rec = auto_ready = false;
        ring_reset();
        rec_free();
        pthread_mutex_unlock(&rx_lock);
        show_listening();
    } else {
        show_standby();
    }
}

/* ---- Window ---- */

static void construct_cb(lv_obj_t *parent)
{
    dialog.obj = dialog_init(parent);
    lv_obj_set_size(dialog.obj, 795, DIALOG_H);

    app_frame_create(&sstv_frame, dialog.obj);
    lv_label_set_text(sstv_frame.status, SSTV_HELP_LINE);
    header();

    lv_obj_t *body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (param_f_get(cfg_pwr) > MAX_PWR) {
        radio_set_pwr(MAX_PWR);
        msg_schedule_text_fmt("Power is limited to %0.0fW", MAX_PWR);
    }

    /* white sheet with the picture, at the left of the body */
    sstv_canvas = lv_canvas_create(dialog.obj);
    lv_canvas_set_buffer(sstv_canvas, sstv_pixels, SSTV_PIC_W, SSTV_PIC_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_move_background(body);
    lv_obj_align(sstv_canvas, LV_ALIGN_TOP_LEFT, SSTV_PIC_X, SSTV_PIC_Y);
    lv_obj_set_size(sstv_canvas, SSTV_PIC_W, SSTV_PIC_H);
    lv_canvas_fill_bg(sstv_canvas, lv_color_white(), LV_OPA_COVER);
    /* the canvas holds the focus so the volume keys reach sstv_key_cb() */
    lv_obj_add_event_cb(sstv_canvas, sstv_key_cb, LV_EVENT_KEY, NULL);
    sstv_focus_picture();

    sstv_text = lv_label_create(dialog.obj);
    lv_obj_set_style_text_color(sstv_text, TC(text), 0);
    lv_obj_set_style_text_font(sstv_text, &sony_22, 0);
    lv_obj_set_width(sstv_text, APP_FRAME_W - SSTV_PIC_W - 30);
    lv_obj_set_pos(sstv_text, SSTV_PIC_X + SSTV_PIC_W + 15, APP_FRAME_BODY_Y + 10);
    lv_obj_set_style_text_align(sstv_text, LV_TEXT_ALIGN_LEFT, 0);

    sstv_bar = sstv_frame.bar;
    lv_obj_set_style_bg_color(sstv_bar, TC(ok_bar), LV_PART_INDICATOR);
    /* VISION: the picture the window's height (app_sstv_int.h); the app and frequency, the state,
     * the bar and the news in a column at its right; no help line (the buttons say it) */
    if (theme_shape.vision) {
        lv_obj_t *col = lv_obj_create(dialog.obj);
        lv_obj_t *move[] = {sstv_frame.head_left, sstv_frame.head_right, sstv_bar, sstv_text};

        lv_obj_remove_style_all(col);
        lv_obj_set_pos(col, SSTV_COL_X, 10);
        lv_obj_set_size(col, SSTV_COL_W, DIALOG_H - 20);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 8, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        for (int i = 0; i < 4; i++) {
            lv_obj_set_parent(move[i], col);
            lv_obj_set_width(move[i], LV_PCT(100));
        }
        lv_label_set_long_mode(sstv_frame.head_right, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(sstv_frame.head_right, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_add_flag(sstv_frame.status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(body, APP_FRAME_X, 8);
        lv_obj_set_height(body, DIALOG_H - 16);
    }
    show_standby();

    btn_restart.label = restart ? "Auto RX\nRestart ON" : "Auto RX\nRestart OFF";
    buttons_disabled(&btn_restart, false);
    btn_denoise.label = params.sstv_denoise.x ? "Denoise\nON" : "Denoise\nOFF";

    pic_shown = 0;
    pic_ready = false;
    pic_rows = 0;
    auto_clear = auto_drop = auto_label = false;
    timer = lv_timer_create(timer_cb, 30, NULL);
}

static void destruct_cb(void)
{
    /* closing resets the receivers: stop the audio first so audio_cb() does
     * not write into buffers being freed */
    pthread_mutex_lock(&rx_lock);
    auto_on = auto_rec = auto_ready = false;
    man_rec = man_ready = false;
    pthread_mutex_unlock(&rx_lock);

    sstv_tx_close();                      /* never leave with the transmitter keyed */
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    if (worker_on) {                      /* a decoder may still use the buffers */
        pthread_join(worker, NULL);
        worker_on = false;
    }

    /* a decoder that just ended may have started listening again */
    pthread_mutex_lock(&rx_lock);
    auto_on = auto_rec = auto_ready = auto_dec = auto_clear = auto_drop = auto_label = false;
    ring_reset();
    man_rec = man_ready = man_dec = man_locked = man_lock_note = false;
    man_fail = NULL;
    auto_rows_on = man_rows_on = false;
    auto_unsupported = -1;
    pthread_mutex_unlock(&rx_lock);
    rec_free();
    man_free();

    working = false;
    pic_ready = false;
    pic_rows = 0;
    pic_shown = 0;
    saved_note = false;
    saved_name[0] = 0;

    /* the buttons outlive the window: back to their first state */
    sstv_mode = SSTV_ROBOT8;
    sstv_mode_auto = true;
    btn_auto.label = "Start\nAuto RX";
    btn_manual.label = "Start\nManual RX";
    sstv_btn_mode.label = "RX Mode\nAuto";
    page1.items[3] = &sstv_btn_mode;
    sstv_gallery_close();
    radio_set_pwr(param_f_get(cfg_pwr));   /* the power cap ends with the app */

    sstv_canvas = NULL;
    sstv_text = NULL;
    sstv_bar = NULL;
    sstv_frame.head_left = sstv_frame.head_right = sstv_frame.status = NULL;   /* went with the window */
    freq_list_close();
}


/* ---- At boot: the TX pictures and texts we ship, once, never over the user's files ---- */

#define BUNDLED      BOARD_SHARE "/sstv"
#define INSTALLED    SSTV_DIR "/.resources-v1"

static bool copy_new(const char *from, const char *to)
{
    if (access(to, F_OK) == 0) return true;

    FILE  *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    char   buf[4096];
    size_t n;
    bool   ok = in && out;

    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in);
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "SSTV: cannot copy %s to %s: %s\n", from, to, strerror(errno));
        if (out) unlink(to);
    }
    return ok;
}

void sstv_install_files(void)
{
    static const char *const files[] = {
        "tx_pics/defaultpic1.png", "tx_pics/defaultpic2.png", "tx_pics/defaultpic3.png", "tx_pics/defaultpic4.png",
        "tx_text/73.sstvtxt", "tx_text/CQ REPLY.sstvtxt", "tx_text/PSEQSL.sstvtxt",
        "tx_text/R454QRM.sstvtxt", "tx_text/R575QSB.sstvtxt", "tx_text/R595.sstvtxt",
    };
    static const char *const dirs[] = { SSTV_DIR, SSTV_DIR "/tx_pics", SSTV_DIR "/tx_text" };
    char from[256], to[256];

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        if (mkdir(dirs[i], 0775) != 0 && errno != EEXIST) {
            fprintf(stderr, "SSTV: cannot make %s: %s\n", dirs[i], strerror(errno));
            return;
        }
    if (access(INSTALLED, F_OK) == 0) return;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(from, sizeof(from), BUNDLED "/%s", files[i]);
        snprintf(to, sizeof(to), SSTV_DIR "/%s", files[i]);
        if (!copy_new(from, to)) return;  /* again at the next start, nothing overwritten */
    }
    FILE *f = fopen(INSTALLED, "wb");
    if (f) {
        fputs("SSTV pictures and texts installed\n", f);
        fclose(f);
    }
}

