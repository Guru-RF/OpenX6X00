/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax app: reception in Auto (APT start/stop) or
 *  Cont, live view with Align and Tilt, faxes saved as PNG in /mnt/wefax, a
 *  browser to straighten or delete them, TX of a picture.
 */

#include "screen_main.h"
#include "board.h"
#include "wefax_window.h"
#include "freq_list.h"
#include "wefax/decoder.h"
#include "wefax/encoder.h"
#include "tx_player.h"
#include "msg_box.h"
#include "audio.h"
#include "mfk_control.h"
#include "key_input.h"
#include "cfg/cfg_api.h"
#include "gui_styles.h"
#include "theme.h"
#include "radio.h"
#include "gui_events.h"
#include "app_frame.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <png.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define BROWSER_FILES   128

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
static void browser_cb(button_data_t *data);
static void fix_align_cb(button_data_t *data);
static void fix_tilt_cb(button_data_t *data);
static void done_cb(button_data_t *data);

static void browser_close(void);
static void draw_live(void);

/* ---- state ---- */

static wefax_decoder_t *decoder;
static uint32_t gen_seen;           /* Auto start generation already handled here */
static bool     fax_on;             /* Auto: the GUI took the confirmed fax, rows are kept */

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
static lv_timer_t *view_timer, *browser_save_timer;
static bool       align_on, tilt_on; /* the encoder turns live Align / Tilt */

/* PNG written by a worker, the LVGL thread only takes a copy */
typedef struct {
    uint8_t  *img;
    unsigned  rows;
    int       align, tilt;
    char      path[128];
} save_job_t;

static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static bool save_busy, save_over, save_ok;

/* browser of the saved faxes (page 3) */
static lv_obj_t *table;
static char     *files[BROWSER_FILES];
static unsigned  files_n;
static bool      br_open, br_editing, br_align_on, br_tilt_on, br_modified;
static int       br_align, br_tilt;
static uint8_t  *pic;
static unsigned  pic_w, pic_h;
static char      pic_path[256];

/* TX (web remote): a worker plays the picture through tx_player, no RX meanwhile */
static pthread_t     tx_thread;
static bool          tx_thread_on;
static volatile bool tx_running, tx_abort;
static volatile int  tx_progress, tx_result;
static char          tx_path[192];


/* ---- buttons: F1 goes round the 4 pages ---- */

static button_data_t btn_page_1  = {.type = BTN_TEXT, .label = "(FAX 1:4)", .press = page_2_cb};
static button_data_t btn_freq    = {.type = BTN_TEXT, .label = "Freq.", .press = freq_cb};
static button_data_t btn_rx_mode = {.type = BTN_TEXT_FN, .label_fn = rx_mode_label, .press = rx_mode_cb};
static button_data_t btn_clear   = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};
static button_data_t btn_close   = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

static button_data_t btn_page_2 = {.type = BTN_TEXT, .label = "(FAX 2:4)", .press = page_3_cb};
static button_data_t btn_align  = {.type = BTN_TEXT_FN, .label_fn = align_label, .press = align_cb,
                                   .ctrl = CTRL_WEFAX_ALIGN, .encoder_allowed = true};
static button_data_t btn_tilt   = {.type = BTN_TEXT_FN, .label_fn = tilt_label, .press = tilt_cb,
                                   .ctrl = CTRL_WEFAX_TILT, .encoder_allowed = true};
static button_data_t btn_view   = {.type = BTN_TEXT, .label = "View\nBig", .press = view_cb};
static button_data_t btn_save   = {.type = BTN_TEXT, .label = "Save", .press = save_cb};

static button_data_t btn_page_3    = {.type = BTN_TEXT, .label = "(FAX 3:4)", .press = page_4_cb};
static button_data_t btn_browser   = {.type = BTN_TEXT, .label = "Fax\nBrowser", .press = browser_cb};
static button_data_t btn_fix_align = {.type = BTN_TEXT, .label = "Fix\nAlign", .press = fix_align_cb, .disabled = true};
static button_data_t btn_fix_tilt  = {.type = BTN_TEXT, .label = "Fix\nTilt", .press = fix_tilt_cb, .disabled = true};
static button_data_t btn_done      = {.type = BTN_TEXT, .label = "Done", .press = done_cb, .disabled = true};

/* page 4: TX power */
static button_data_t btn_page_4 = {.type = BTN_TEXT, .label = "(FAX 4:4)", .press = page_1_cb};

static buttons_page_t page_1 = {.items = {&btn_page_1, &btn_freq, &btn_rx_mode, &btn_clear, &btn_close}};
static buttons_page_t page_2 = {.items = {&btn_page_2, &btn_align, &btn_tilt, &btn_view, &btn_save}};
static buttons_page_t page_3 = {.items = {&btn_page_3, &btn_browser, &btn_fix_align, &btn_fix_tilt, &btn_done}};
static buttons_page_t page_4 = {.items = {&btn_page_4, &btn_dig_pwr}};

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
static int tilt_px(int64_t from_newest, int tilt, unsigned width)
{
    return (int)(from_newest * tilt * VIEW_W / (1000LL * width));
}

static void fill_view(lv_color_t c)
{
    for (unsigned i = 0; i < VIEW_W * VIEW_H; i++)
        view_buf[i] = c;
}

static void put_px(unsigned x, unsigned y, uint8_t g)
{
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

/* Empty view, kept rows and the row in the box */
static void clear_fax(void)
{
    fill_view(lv_color_make(255, 255, 255));
    shown_acc = shown_n = shown_head = 0;
    if (canvas)
        lv_obj_invalidate(canvas);

    pthread_mutex_lock(&row_lock);
    row_full = false;
    pthread_mutex_unlock(&row_lock);

    store_n = store_head = 0;
}

/* ---- drawing ---- */

/* Full: every kept row, fitted in the body, newest at the bottom */
static void draw_full(void)
{
    if (!canvas)
        return;

    fill_view(lv_color_white());
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

    fill_view(lv_color_make(255, 255, 255));
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

/* Browser: the opened picture with its own corrections, fitted and centred on black */
static void draw_picture(void)
{
    if (!pic || !pic_w || !pic_h)
        return;

    unsigned w = VIEW_W, h = (unsigned)((uint64_t)pic_h * VIEW_W / pic_w);

    if (h > VIEW_H) {
        h = VIEW_H;
        w = (unsigned)((uint64_t)pic_w * VIEW_H / pic_h);
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;

    unsigned x0 = (VIEW_W - w) / 2, y0 = (VIEW_H - h) / 2;

    fill_view(lv_color_make(0, 0, 0));
    for (unsigned y = 0; y < h; y++) {
        unsigned sy = (unsigned)((uint64_t)y * pic_h / h);

        if (sy >= pic_h)
            sy = pic_h - 1;

        int shift = tilt_px((int64_t)sy - (int64_t)(pic_h - 1), br_tilt, pic_w);

        for (unsigned x = 0; x < w; x++) {
            unsigned dx = (unsigned)((uint64_t)x * pic_w / w);

            if (dx >= pic_w)
                dx = pic_w - 1;
            put_px(x0 + x, y0 + y, pic[(size_t)sy * pic_w + (unsigned)wrap_x((int)dx - br_align - shift, (int)pic_w)]);
        }
    }
    if (canvas)
        lv_obj_invalidate(canvas);
}

/* ---- PNG ---- */

/* Grey PNG of img (w x h) with Align and Tilt applied as on screen */
static bool png_write_fixed(FILE *fp, const uint8_t *img, unsigned w, unsigned h, int align, int tilt)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    uint8_t    *line = malloc(w);

    if (!png || !info || !line) {
        png_destroy_write_struct(&png, info ? &info : NULL);
        free(line);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        free(line);
        return false;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *src   = img + (size_t)y * w;
        int            shift = tilt_px((int64_t)y - (int64_t)(h - 1), tilt, w);

        for (unsigned x = 0; x < w; x++)
            line[x] = src[wrap_x((int)x - align - shift, (int)w)];
        png_write_row(png, line);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    free(line);
    return true;
}

static void *save_worker(void *arg)
{
    save_job_t *job = arg;
    bool        ok  = false;
    FILE       *fp;

    if (mkdir(SAVE_DIR, 0775) != 0 && errno != EEXIST) {
        printf("WEFAX: can't make %s: %s\n", SAVE_DIR, strerror(errno));
    } else if (!(fp = fopen(job->path, "wb"))) {
        printf("WEFAX: can't write %s: %s\n", job->path, strerror(errno));
    } else {
        ok = png_write_fixed(fp, job->img, WEFAX_WIDTH, job->rows, job->align, job->tilt);
        if (fclose(fp) != 0)
            ok = false;
        if (!ok) {
            remove(job->path);
            printf("WEFAX: writing %s failed\n", job->path);
        }
    }

    if (ok) {
        printf("WEFAX: saved %s (%u x %u)\n", job->path, WEFAX_WIDTH, job->rows);
    }

    free(job->img);
    free(job);

    pthread_mutex_lock(&save_lock);
    save_busy = false;
    save_ok   = ok;
    save_over = true;
    pthread_mutex_unlock(&save_lock);
    return NULL;
}

/* Copy the kept fax (oldest row first) and hand it to a worker; the kept rows can go right after */
static bool save_fax(void)
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
    struct tm   tm;

    if (!job || !(job->img = malloc((size_t)store_n * WEFAX_WIDTH)) || !localtime_r(&now, &tm)) {
        printf("WEFAX: can't save, no memory or clock\n");
        if (job)
            free(job->img);
        free(job);
        return false;
    }
    job->rows  = store_n;
    job->align = (int)param_i_get(cfg_wefax_align);
    job->tilt  = (int)param_i_get(cfg_wefax_tilt);
    for (unsigned r = 0; r < store_n; r++)
        memcpy(job->img + (size_t)r * WEFAX_WIDTH, store_row(r), WEFAX_WIDTH);
    snprintf(job->path, sizeof(job->path), SAVE_DIR "/wefax-%04d%02d%02d-%02d%02d%02d.png", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

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
    pthread_mutex_lock(&save_lock);
    bool over = save_over, ok = save_ok;
    save_over = false;
    pthread_mutex_unlock(&save_lock);

    if (over)
        channel_overlay_show(ok ? "Fax saved" : "Fax save failed");
}

/* ---- Auto ---- */

/* A new start over a fax the GUI took: the old one is over even if its stop was missed.
 * The decoder already follows the new fax, it is not reset. */
static bool auto_keep_previous(void)
{
    if (store_n && !save_fax()) {
        printf("WEFAX AUTO: the previous fax could not be saved, kept\n");
        return false;
    }
    clear_fax();
    fax_on = false;
    return true;
}

/* Stop tone or kept rows full: save, then Auto waits for the next start */
static void auto_fax_over(void)
{
    if (!decoder)
        return;
    if (store_n && !save_fax()) {
        printf("WEFAX AUTO: the fax could not be saved, kept\n");
        return;
    }
    gen_seen = 0;
    fax_on   = false;
    wefax_decoder_set_mode(decoder, WEFAX_MODE_AUTO);      /* one reset only: it keeps the pause after the stop */
    clear_fax();
}

/* The decoder starts over (Clear, after a TX) */
static void rx_restart(void)
{
    wefax_decoder_reset(decoder);
    gen_seen = 0;
    fax_on   = false;
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

/* ---- LVGL timer ---- */

/* Station and frequency on the left, receiver state on the right */
static void header_update(void)
{
    char        l[64], r[48];
    int32_t     f    = cparam_i_get(cfg_fg_freq) + WEFAX_CENTER_HZ;     /* USB */
    const char *name = "WeFax";

    for (int i = 0; i < STATIONS_N; i++)
        if (!stations[i].group && stations[i].freq_hz == f)
            name = stations[i].name;
    snprintf(l, sizeof(l), "%s   %.1f kHz", name, f / 1000.0);
    if (tx_running)
        snprintf(r, sizeof(r), "#%06x TX %i%%#", theme_hex(TC_tx), tx_progress / 10);
    else
        snprintf(r, sizeof(r), "%s", decoder && wefax_decoder_get_mode(decoder) == WEFAX_MODE_CONT ? "RX Cont" : "RX Auto");

    if (strcmp(l, lv_label_get_text(frame.head_left)))
        lv_label_set_text(frame.head_left, l);
    if (strcmp(r, lv_label_get_text(frame.head_right)))
        lv_label_set_text(frame.head_right, r);
}

static void view_timer_cb(lv_timer_t *t)
{
    (void)t;


    if (frame.head_left)
        header_update();
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
            channel_overlay_show("Fax Started");
        }

        /* the stop tone: file I/O here, never in the audio thread */
        if (wefax_decoder_auto_stop_detected(decoder)) {
            auto_fax_over();
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

    /* a fax longer than the kept rows: save it rather than lose its top */
    if (auto_mode() && fax_on && store_n >= STORE_ROWS) {
        printf("WEFAX AUTO: %u rows kept, saving\n", store_n);
        auto_fax_over();
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

    /* with the browser open the history goes on, the drawing waits for it to close */
    if (!br_open)
        draw_live();        /* all rows: Tilt depends on the distance from the newest */
}

/* ---- view ---- */

/* Small: a low panel over the buttons, the waterfall above; Big/Full: the common app look */
static void apply_view(void)
{
    if (!dialog.obj || !canvas || !viewport)
        return;

    if (view == VIEW_SMALL) {
        lv_obj_set_size(dialog.obj, 795, 182);
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
            lv_obj_set_size(panel, 795, 182);
            lv_obj_center(panel);
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_background(panel);
        }
    } else {
        lv_obj_set_size(dialog.obj, 795, DIALOG_H);
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

    decoder = wefax_decoder_create();
    if (!decoder) {
        fprintf(stderr, "WEFAX: can't create the decoder\n");
    } else {
        wefax_decoder_set_row_callback(decoder, row_cb, NULL);
        wefax_decoder_set_mode(decoder, WEFAX_MODE_AUTO);      /* open in Auto, as the header and the RX button say */
    }

    dialog.obj = dialog_init(parent);
    lv_obj_set_size(dialog.obj, 795, DIALOG_H);
    lv_obj_align(dialog.obj, LV_ALIGN_TOP_MID, 0, DIALOG_Y);

    /* no background image of dialog_init: its own white edges would show around ours */
    lv_obj_set_style_bg_img_src(dialog.obj, NULL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dialog.obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog.obj, 0, LV_PART_MAIN);

    /* transparent clip of the canvas (in Small it shows only the bottom of it) */
    viewport = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(viewport);
    lv_obj_set_size(viewport, 795, DIALOG_H);
    lv_obj_center(viewport);
    lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(viewport, LV_OBJ_FLAG_CLICKABLE);

    canvas = lv_canvas_create(viewport);
    lv_canvas_set_buffer(canvas, view_buf, VIEW_W, VIEW_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(canvas, VIEW_W, VIEW_H);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);

    /* the panel behind Small: panel colours flat (the panel image has its own outline), one border */
    panel = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(panel);
    lv_obj_add_style(panel, &panel_style, 0);
    lv_obj_set_style_bg_img_src(panel, NULL, LV_PART_MAIN);
    lv_obj_set_size(panel, 795, DIALOG_H);
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
    lv_label_set_text(frame.status, "MFK: align   Freq.: stations   View: Small shows the waterfall");

    /* the canvas takes the keys: the main screen's key handler is off while a dialog is open */
    lv_group_add_obj(keyboard_group, canvas);
    lv_obj_add_event_cb(canvas, app_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(canvas);
    lv_group_set_editing(keyboard_group, true);

    clear_fax();
    apply_view();
    view_timer = lv_timer_create(view_timer_cb, VIEW_MS, NULL);
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
    if (browser_save_timer) {
        lv_timer_del(browser_save_timer);
        browser_save_timer = NULL;
    }

    align_on = tilt_on = false;
    browser_close();
    canvas          = NULL;
    viewport        = NULL;
    panel           = NULL;
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
}

static void app_key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    /* the volume knob, whatever the MFK does */
    switch (key) {
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            return;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            return;
        default:
            break;
    }

    if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT)
        return;

    bool left = key == LV_KEY_LEFT;

    if (br_align_on && br_editing) {
        br_align += left ? -8 : 8;
        while (br_align < -(int)(pic_w / 2))
            br_align += (int)pic_w;
        while (br_align > (int)(pic_w / 2))
            br_align -= (int)pic_w;
    } else if (br_tilt_on && br_editing) {
        br_tilt += left ? -1 : 1;
        if (br_tilt < -400)
            br_tilt = -400;
        if (br_tilt > 400)
            br_tilt = 400;
    } else if (align_on) {
        mfk_update(left ? -8 : 8);
        buttons_refresh(&btn_align);
        draw_live();
        return;
    } else if (tilt_on) {
        mfk_update(left ? -1 : 1);
        buttons_refresh(&btn_tilt);
        draw_live();
        return;
    } else {
        return;
    }
    draw_picture();

    /* the browser picture differs from its file only with a correction: Done becomes Save */
    br_modified    = br_align || br_tilt;
    btn_done.label = br_modified ? "Save\nFax" : "Done";
    if (btn_done.disp_btn)
        buttons_refresh(&btn_done);
}

/* ---- browser ---- */

static int newest_first(const void *a, const void *b)
{
    return strcmp(*(const char *const *)b, *(const char *const *)a);
}

static void files_free(void)
{
    for (unsigned i = 0; i < files_n; i++) {
        free(files[i]);
        files[i] = NULL;
    }
    files_n = 0;
}

/* wefax-*.png in SAVE_DIR, newest first */
static void files_read(void)
{
    files_free();

    DIR *dir = opendir(SAVE_DIR);

    if (!dir)
        return;

    struct dirent *de;

    while ((de = readdir(dir)) && files_n < BROWSER_FILES) {
        size_t len = strlen(de->d_name);

        if (len >= 5 && !strcmp(de->d_name + len - 4, ".png") && !strncmp(de->d_name, "wefax-", 6) &&
            (files[files_n] = strdup(de->d_name)))
            files_n++;
    }
    closedir(dir);
    qsort(files, files_n, sizeof(files[0]), newest_first);
}

/* Page 3 buttons: on a picture Fix Align, Fix Tilt and Done/Save; on the list F5 deletes */
static void browser_buttons(bool picture)
{
    if (picture) {
        br_modified    = br_align || br_tilt;
        btn_done.label = br_modified ? "Save\nFax" : "Done";
        if (btn_done.disp_btn)
            buttons_refresh(&btn_done);
        buttons_disabled(&btn_fix_align, false);
        buttons_disabled(&btn_fix_tilt, false);
        buttons_disabled(&btn_done, false);
        return;
    }

    br_modified    = false;
    btn_done.label = "Delete\nFax";
    buttons_disabled(&btn_fix_align, true);
    buttons_disabled(&btn_fix_tilt, true);
    buttons_disabled(&btn_done, files_n == 0);
    if (btn_done.disp_btn)
        buttons_refresh(&btn_done);
}

static void table_fill(void)
{
    if (!table)
        return;

    if (!files_n) {
        lv_table_set_row_cnt(table, 1);
        lv_table_set_cell_value(table, 0, 0, "No saved faxes");
    } else {
        lv_table_set_row_cnt(table, files_n);
        for (unsigned i = 0; i < files_n; i++)
            lv_table_set_cell_value(table, i, 0, files[i]);
    }
    lv_obj_invalidate(table);
}

/* The file selected in the list, or -1 */
static int table_selected(void)
{
    uint16_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;

    if (!table || !files_n)
        return -1;
    lv_table_get_selected_cell(table, &row, &col);
    return row == LV_TABLE_CELL_NONE || row >= files_n ? -1 : (int)row;
}

static void table_focus(void)
{
    lv_group_add_obj(keyboard_group, table);
    lv_group_focus_obj(table);
    lv_group_set_editing(keyboard_group, true);
}

static void picture_free(void)
{
    free(pic);
    pic         = NULL;
    pic_w       = 0;
    pic_h       = 0;
    pic_path[0] = '\0';
    br_editing  = false;
    br_align_on = false;
    br_tilt_on  = false;
    br_align    = 0;
    br_tilt     = 0;
    br_modified = false;
}

/* Any PNG in SAVE_DIR, as 8-bit grey */
static bool picture_open(const char *name)
{
    char path[256];

    snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, name);

    FILE *fp = fopen(path, "rb");

    if (!fp) {
        printf("WEFAX: can't open %s: %s\n", path, strerror(errno));
        return false;
    }

    png_structp png  = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    uint8_t *volatile img = NULL, *volatile line = NULL;      /* freed after a longjmp */

    if (!png || !info) {
        png_destroy_read_struct(&png, info ? &info : NULL, NULL);
        fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        free(img);
        free(line);
        printf("WEFAX: %s is not a readable PNG\n", path);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    int         depth = png_get_bit_depth(png, info), type = png_get_color_type(png, info);

    if (!w || !h || w > 10000 || h > 10000) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        printf("WEFAX: %s: bad size\n", path);
        return false;
    }

    /* everything to 8-bit RGB(A), then grey */
    if (depth == 16)
        png_set_strip_16(png);
    if (type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    type = png_get_color_type(png, info);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_read_update_info(png, info);

    size_t rowbytes = png_get_rowbytes(png, info);
    int    ch       = png_get_channels(png, info);

    img  = malloc((size_t)w * h);
    line = malloc(rowbytes);
    if (ch < 3 || !rowbytes || !img || !line) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        free(img);
        free(line);
        printf("WEFAX: %s: unsupported or no memory\n", path);
        return false;
    }

    for (png_uint_32 y = 0; y < h; y++) {
        png_read_row(png, line, NULL);
        for (png_uint_32 x = 0; x < w; x++) {
            const uint8_t *p = line + (size_t)x * ch;

            img[(size_t)y * w + x] = (uint8_t)((77U * p[0] + 150U * p[1] + 29U * p[2]) >> 8);
        }
    }
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    free(line);

    picture_free();
    pic   = img;
    pic_w = w;
    pic_h = h;
    snprintf(pic_path, sizeof(pic_path), "%s", path);
    draw_picture();

    if (table) {
        lv_group_remove_obj(table);
        lv_obj_add_flag(table, LV_OBJ_FLAG_HIDDEN);
    }
    if (canvas) {
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        focus_canvas();
        apply_view();
        lv_obj_invalidate(canvas);
    }
    br_editing = true;
    browser_buttons(true);
    printf("WEFAX: opened %s (%u x %u)\n", path, pic_w, pic_h);
    return true;
}

static void table_pressed_cb(lv_event_t *e)
{
    (void)e;

    int i = table_selected();

    if (i >= 0 && picture_open(files[i]))
        channel_overlay_show("Editing saved fax");
}

static void browser_open(void)
{
    if (br_open)
        return;

    files_read();
    browser_buttons(false);
    if (canvas) {
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        lv_group_remove_obj(canvas);
    }
    if (panel && view == VIEW_SMALL) {     /* Big/Full: the common look has no panel */
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_background(panel);
    }

    table = lv_table_create(dialog.obj);
    if (view == VIEW_SMALL) {
        lv_obj_set_size(table, VIEW_W, 142);
        lv_obj_align(table, LV_ALIGN_TOP_MID, 0, 20);
    } else {
        lv_obj_set_size(table, VIEW_W, APP_FRAME_BODY_H - 16);
        lv_obj_set_pos(table, APP_FRAME_X + (APP_FRAME_W - VIEW_W) / 2, APP_FRAME_BODY_Y + 8);
    }
    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, VIEW_W - 10);
    lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, TC(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &sony_24, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 12, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text_mute), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_EDITED);
    table_fill();
    lv_obj_add_event_cb(table, table_pressed_cb, LV_EVENT_PRESSED, NULL);
    table_focus();

    br_open = true;
    browser_buttons(false);
    btn_browser.label = "Close\nBrowser";
    if (btn_browser.disp_btn)
        buttons_refresh(&btn_browser);
}

static void browser_close(void)
{
    if (table) {
        lv_group_remove_obj(table);
        lv_obj_del(table);
        table = NULL;
    }
    files_free();
    browser_buttons(false);
    picture_free();
    br_open = false;

    btn_done.label = "Done";
    buttons_disabled(&btn_done, true);
    if (btn_done.disp_btn)
        buttons_refresh(&btn_done);
    btn_browser.label = "Fax\nBrowser";

    /* the live fax back over the picture, with the current layout */
    draw_live();
    if (canvas) {
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        apply_view();
        focus_canvas();
        lv_obj_invalidate(dialog.obj);
        lv_obj_invalidate(canvas);
    }
}

/* From a picture back to the list */
static void browser_list(void)
{
    browser_buttons(false);
    if (canvas) {
        lv_group_remove_obj(canvas);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    }
    picture_free();
    files_read();
    if (table) {
        lv_obj_clear_flag(table, LV_OBJ_FLAG_HIDDEN);
        table_fill();
        browser_buttons(false);
        table_focus();
        lv_obj_invalidate(table);
    }
}

static void browser_delete(void)
{
    if (!br_open || br_editing)
        return;

    int i = table_selected();

    if (i < 0)
        return;

    char path[256];

    snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, files[i]);
    if (unlink(path) != 0) {
        printf("WEFAX: can't delete %s: %s\n", path, strerror(errno));
        return;
    }
    printf("WEFAX: deleted %s\n", path);

    /* same table: the selection stays on the row, now the next fax (LVGL 8 can't set it) */
    files_read();
    table_fill();
    browser_buttons(false);
    lv_group_focus_obj(table);
    lv_group_set_editing(keyboard_group, true);
}

/* The corrected picture over its own file: a temporary file renamed once complete */
static bool picture_save(void)
{
    if (!pic || !pic_w || !pic_h || !pic_path[0])
        return false;

    char  tmp[sizeof(pic_path) + 8];
    FILE *fp;

    snprintf(tmp, sizeof(tmp), "%s.tmp", pic_path);
    if (!(fp = fopen(tmp, "wb"))) {
        printf("WEFAX: can't write %s: %s\n", tmp, strerror(errno));
        return false;
    }

    bool ok = png_write_fixed(fp, pic, pic_w, pic_h, br_align, br_tilt);

    if (fclose(fp) != 0 || !ok) {
        remove(tmp);
        printf("WEFAX: writing %s failed\n", tmp);
        return false;
    }
    if (rename(tmp, pic_path) != 0) {
        remove(tmp);
        printf("WEFAX: can't rename %s: %s\n", tmp, strerror(errno));
        return false;
    }
    printf("WEFAX: saved %s (%u x %u), align %d tilt %d\n", pic_path, pic_w, pic_h, br_align, br_tilt);
    return true;
}

static void picture_save_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    browser_save_timer = NULL;

    if (br_open && br_editing && pic && picture_save()) {
        browser_list();
        channel_overlay_show("Fax saved");
    }
}

/* ---- buttons ---- */

static void page_1_cb(button_data_t *data)
{
    (void)data;
    browser_close();
    align_on = tilt_on = false;
    buttons_load_page(&page_1);
}

static void page_2_cb(button_data_t *data)
{
    (void)data;
    browser_close();
    buttons_load_page(&page_2);
}

static void page_3_cb(button_data_t *data)
{
    (void)data;
    align_on = tilt_on = false;
    buttons_load_page(&page_3);
}

static void page_4_cb(button_data_t *data)
{
    (void)data;
    browser_close();
    buttons_load_page(&page_4);
}

static void close_cb(button_data_t *data)
{
    (void)data;
    dialog_destruct();
}

static void freq_cb(button_data_t *data)
{
    (void)data;
    if (freq_list_is_open())
        freq_list_close();
    else
        freq_list_open(app_parent, "WEFAX - stations", stations, STATIONS_N, WEFAX_CENTER_HZ, NULL);
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
    if (decoder && save_fax()) {
        wefax_decoder_reset(decoder);
        gen_seen = 0;
        fax_on   = false;
        clear_fax();
    }
}

static void browser_cb(button_data_t *data)
{
    (void)data;
    if (!br_open) {
        browser_open();
        return;
    }
    browser_close();
    if (btn_browser.disp_btn)
        buttons_refresh(&btn_browser);
}

static void fix_align_cb(button_data_t *data)
{
    (void)data;
    if (!br_open || !br_editing || !pic)
        return;
    br_align_on = true;
    br_tilt_on  = false;
    align_on = tilt_on = false;
    if (canvas) {
        lv_group_focus_obj(canvas);
        lv_group_set_editing(keyboard_group, true);
    }
}

static void fix_tilt_cb(button_data_t *data)
{
    (void)data;
    if (!br_open || !br_editing || !pic)
        return;
    br_tilt_on  = true;
    br_align_on = false;
    align_on = tilt_on = false;
    if (canvas) {
        lv_group_focus_obj(canvas);
        lv_group_set_editing(keyboard_group, true);
    }
}

/* F5 of page 3: Delete on the list; on a picture Done, or Save with a correction */
static void done_cb(button_data_t *data)
{
    (void)data;

    if (br_open && !br_editing) {
        browser_delete();
        return;
    }
    if (!br_open || !br_editing || !pic)
        return;

    br_modified = br_align || br_tilt;
    if (!br_modified) {
        browser_buttons(true);
        browser_list();
        return;
    }
    if (browser_save_timer)
        return;

    /* the overlay first: the PNG is written on the next timer pass */
    channel_overlay_show("Saving fax...");
    browser_save_timer = lv_timer_create(picture_save_timer_cb, 50, NULL);
}
