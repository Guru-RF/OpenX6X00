/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 */

/* Screen + RX audio recorder: /mnt/screenrec/screen-YYYYMMDD-HHMMSS.avi
 * (MJPEG video, PCM s16 mono audio).
 *
 * LVGL thread: the display flush (fbdev or DRM) is wrapped to keep a copy of the screen; a timer
 * puts a snapshot in a small queue SCREEN_REC_FPS times a second, or drops it
 * when the queue is full.
 * Audio thread: only copies samples into a ring.
 * Writer thread (nice +10): JPEG encode and file write. Frames are placed by
 * their timestamps: late ones are dropped, gaps are filled with the previous
 * frame, so audio and video stay in sync. */

#include "lv_drivers/display/drm.h"
#include "board.h"
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/resource.h>
#include <jpeglib.h>

#include "screen_rec.h"
#include "remote_server.h"
#include "avi_writer.h"
#include "audio.h"
#include "recorder.h"
#include "radio.h"
#include "buttons.h"
#include "msg.h"
#include "styles.h"
#include "theme.h"
#include "params/params.h"
#include "lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"

#define SCREEN_REC_FPS      8
#define SCREEN_REC_QUALITY  70
#ifndef SCREEN_REC_DIR
#define SCREEN_REC_DIR      BOARD_DATA "/screenrec"
#endif
#define SCREEN_REC_MIN_MB   50
#define SCREEN_REC_MB_MIN   45              /* about 42 MB a minute measured on the radio */
#define SCREEN_REC_WARN_MIN 10              /* "almost full" when less than this is left */
#define SPLIT_BYTES         3000000000u     /* a new file well before the 4 GB AVI limit */

#define W       800
#define H       480
#define QUEUE   3
#define RING    (1 << 17)       /* audio samples, about 3 s */

enum { IDLE, RUN, STOP };

typedef struct {
    uint8_t     *px;
    int64_t     ms;
} frame_t;

static pthread_mutex_t  mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   cond = PTHREAD_COND_INITIALIZER;

static int              state = IDLE;   /* RUN/STOP set by the LVGL thread, IDLE by the writer */
static int              error;          /* writer: 1 write, 2 encoder, 3 open, 4 card full (file closed) */
static char             path[128];
static int64_t          t0;
static uint32_t         split_frames;   /* file length, set by start() */
static int              files;          /* files written by this recording */
static int              low_min = -1;   /* writer: minutes left on the card, when low */

/* Under mut */
static frame_t          queue[QUEUE];
static int              q_head, q_tail, q_count;
static int16_t          ring[RING];
static size_t           ring_r, ring_w;

/* LVGL thread only */
static uint8_t          *shadow;
static bool             active;
static lv_timer_t       *timer;
static lv_obj_t         *label;
static int              shown = -1;
static int              warned_min = -1;

static int64_t now_ms() {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int get_state() {
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE);
}

static void set_state(int x) {
    __atomic_store_n(&state, x, __ATOMIC_RELEASE);
}

static void request_stop() {
    int run = RUN;

    __atomic_compare_exchange_n(&state, &run, STOP, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);  /* not when the writer is done */
    pthread_cond_signal(&cond);
}

/* Writer thread */

static void jpeg_fail(j_common_ptr c) {
    longjmp(*(jmp_buf *) c->client_data, 1);
}

static bool encode(const uint8_t *px, uint8_t **out, unsigned long *size) {
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr       jerr;
    jmp_buf                     jb;

    c.err = jpeg_std_error(&jerr);
    jerr.error_exit = jpeg_fail;
    jpeg_create_compress(&c);
    c.client_data = &jb;

    if (setjmp(jb)) {
        jpeg_destroy_compress(&c);
        return false;
    }

    jpeg_mem_dest(&c, out, size);
    c.image_width = W;
    c.image_height = H;
    c.input_components = 4;
    c.in_color_space = JCS_EXT_BGRX;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, SCREEN_REC_QUALITY, TRUE);
    c.dct_method = JDCT_IFAST;
    jpeg_start_compress(&c, TRUE);

    while (c.next_scanline < H) {
        JSAMPROW row = (JSAMPROW) (px + c.next_scanline * W * 4);

        jpeg_write_scanlines(&c, &row, 1);
    }

    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    return true;
}

static bool write_audio(avi_t *avi, int16_t *buf, size_t max) {
    size_t n;

    pthread_mutex_lock(&mut);
    n = ring_w - ring_r;
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = ring[(ring_r + i) % RING];
    }
    ring_r += n;
    pthread_mutex_unlock(&mut);

    return n == 0 || avi_write_audio(avi, buf, n);
}

static void make_path() {
    char    name[32];
    time_t  now = time(NULL);

    strftime(name, sizeof(name), "screen-%Y%m%d-%H%M%S.avi", localtime(&now));
    snprintf(path, sizeof(path), SCREEN_REC_DIR "/%s", name);
}

/* MB free on the card, -1 when unknown */
static int64_t free_mb() {
    struct statvfs fs;

    if (statvfs(SCREEN_REC_DIR, &fs) != 0) {
        return -1;
    }
    return (uint64_t) fs.f_bavail * fs.f_frsize / (1024 * 1024);
}

static void * writer_thread(void *arg) {
    static int16_t  abuf[8192];
    uint8_t         *cur = NULL, *prev = NULL;      /* JPEG data */
    unsigned long   cur_n = 0, prev_n = 0;
    uint32_t        next = 0;                       /* next frame index */
    uint32_t        file_start = 0;                 /* first frame index of this file */
    int64_t         checked = 0;                    /* last free space check */
    bool            ok = true;

    setpriority(PRIO_PROCESS, 0, 10);               /* Linux: this thread only */

    avi_t *avi = avi_open(path, W, H, SCREEN_REC_FPS, AUDIO_CAPTURE_RATE);

    if (!avi) {
        error = 3;
        set_state(IDLE);
        return NULL;
    }
    files = 1;

    while (ok && get_state() == RUN) {
        struct timespec ts;
        int             i = -1;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }

        pthread_mutex_lock(&mut);
        if (q_count == 0) {
            pthread_cond_timedwait(&cond, &mut, &ts);
        }
        if (q_count > 0) {
            i = q_tail;
        }
        pthread_mutex_unlock(&mut);

        if (i >= 0) {
            uint32_t    idx = (queue[i].ms * SCREEN_REC_FPS + 500) / 1000;

            if (idx >= next) {
                if (!encode(queue[i].px, &cur, &cur_n)) {
                    error = 2;
                    ok = false;
                }
                if (ok && !prev) {
                    prev = cur;                     /* leading gap: repeat the first frame */
                    prev_n = cur_n;
                    cur = NULL;
                    cur_n = 0;
                    ok = encode(queue[i].px, &cur, &cur_n);
                }
                for (; ok && next < idx; next++) {
                    ok = avi_write_video(avi, prev, prev_n);
                }
                if (ok) {
                    ok = avi_write_video(avi, cur, cur_n);
                    next++;
                }
                uint8_t *t = prev;                  /* keep the buffers, libjpeg reuses them */
                unsigned long tn = prev_n;

                prev = cur;
                prev_n = cur_n;
                cur = t;
                cur_n = tn;
            }

            pthread_mutex_lock(&mut);
            q_tail = (q_tail + 1) % QUEUE;
            q_count--;
            pthread_mutex_unlock(&mut);
        }

        ok = ok && write_audio(avi, abuf, sizeof(abuf) / 2);

        /* Every 5 s: stop with the file closed before the card is full */
        if (ok && now_ms() - checked >= 5000) {
            int64_t mb = free_mb();

            checked = now_ms();

            if (mb >= 0 && mb < SCREEN_REC_MIN_MB) {
                error = 4;
                break;
            }
            low_min = (mb >= 0 && mb < SCREEN_REC_WARN_MIN * SCREEN_REC_MB_MIN) ? mb / SCREEN_REC_MB_MIN : -1;
        }

        /* File length reached: close this file and go on in a new one */
        if (ok && (next - file_start >= split_frames || avi_size(avi) > SPLIT_BYTES)) {
            ok = avi_close(avi);
            avi = NULL;
            if (ok) {
                make_path();
                avi = avi_open(path, W, H, SCREEN_REC_FPS, AUDIO_CAPTURE_RATE);
                if (!avi) {
                    error = 3;
                    break;
                }
                files++;
                file_start = next;
            }
        }
    }

    while (ok && avi && ring_w != ring_r) {
        ok = write_audio(avi, abuf, sizeof(abuf) / 2);
    }

    if ((avi && !avi_close(avi)) || !ok) {
        error = error ? error : 1;
    }

    free(cur);
    free(prev);
    set_state(IDLE);
    return NULL;
}

/* Audio thread */

void screen_rec_put_audio_samples(size_t nsamples, int16_t *samples) {
    if (get_state() != RUN) {
        return;
    }

    pthread_mutex_lock(&mut);
    if (RING - (ring_w - ring_r) >= nsamples) {     /* full: drop */
        for (size_t i = 0; i < nsamples; i++) {
            ring[(ring_w + i) % RING] = samples[i];
        }
        ring_w += nsamples;
    }
    pthread_mutex_unlock(&mut);
}

/* LVGL thread */

void (*screen_rec_out)(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) = fbdev_flush;

void screen_rec_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    remote_flush(area, color_p);       /* the browser remote's viewers */
    if (active && shadow) {
        int32_t x1 = area->x1 < 0 ? 0 : area->x1;
        int32_t x2 = area->x2 >= W ? W - 1 : area->x2;
        int32_t w = area->x2 - area->x1 + 1;

        for (int32_t y = area->y1 < 0 ? 0 : area->y1; y <= area->y2 && y < H; y++) {
            memcpy(shadow + (y * W + x1) * 4, (uint8_t *) color_p + ((y - area->y1) * w + x1 - area->x1) * 4, (x2 - x1 + 1) * 4);
        }
    }
    screen_rec_out(drv, area, color_p);
}

static void timer_cb(lv_timer_t *t) {
    int s = get_state();

    if (active && s == RUN) {
        int64_t ms = now_ms() - t0;
        int     i = -1;

        pthread_mutex_lock(&mut);
        if (q_count < QUEUE) {
            i = q_head;
        }
        pthread_mutex_unlock(&mut);

        if (i >= 0) {                               /* else the writer is late: drop the frame */
            memcpy(queue[i].px, shadow, W * H * 4);
#if USE_DRM
            drm_compose_landscape((uint32_t *) queue[i].px);   /* + the waterfall plane */
#endif
            queue[i].ms = ms;
            pthread_mutex_lock(&mut);
            q_head = (q_head + 1) % QUEUE;
            q_count++;
            pthread_cond_signal(&cond);
            pthread_mutex_unlock(&mut);
        }

        if (ms / 1000 != shown) {
            shown = ms / 1000;
            lv_label_set_text_fmt(label, "REC %02d:%02d", shown / 60, shown % 60);
        }

        int low = __atomic_load_n(&low_min, __ATOMIC_RELAXED);

        if (low >= 0 && low != warned_min) {        /* again each minute less */
            warned_min = low;
            msg_schedule_long_text_fmt("SD card: about %d min left", low);
        }
    } else if (s == IDLE) {                         /* the writer is done, or failed */
        if (active) {
            active = false;
            radio_set_rec_moni(false);
            buttons_screen_rec_update();
        }
        if (error == 4) {
            msg_schedule_long_text_fmt("SD card full, recording saved");
        } else if (error == 1) {
            msg_schedule_long_text_fmt("SD card write error: the last file may be damaged");
        } else if (error == 3) {
            msg_schedule_long_text_fmt("Cannot create the file on the SD card");
        } else if (error) {
            msg_update_text_fmt("Screen rec error");
        } else if (files > 1) {
            msg_update_text_fmt("Saved %d files", files);
        } else {
            msg_update_text_fmt("Saved %s", strrchr(path, '/') + 1);
        }
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_timer_del(timer);
        timer = NULL;
    }
}

/* On an SD card, not the radio's internal eMMC: the block device holding the folder says its type */
static bool on_sd_card(const char *dir) {
    struct stat st;
    char        path[64], type[8] = "";
    FILE       *f;

    if (stat(dir, &st) != 0) {
        return false;
    }
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/../device/type", major(st.st_dev), minor(st.st_dev));
    f = fopen(path, "r");
    if (!f) {
        return false;
    }
    if (!fgets(type, sizeof(type), f)) {
        type[0] = 0;
    }
    fclose(f);
    return strncmp(type, "SD", 2) == 0;
}

static bool start() {
    static const uint8_t split_min[] = { 60, 45, 30, 15, 5 };

    if (get_state() != IDLE || timer) {
        msg_update_text_fmt("Saving previous recording");
        return false;
    }

    mkdir(SCREEN_REC_DIR, 0755);

    if (!on_sd_card(SCREEN_REC_DIR)) {
        msg_update_text_fmt("Screen rec: no SD card");
        return false;
    }
    if (free_mb() < SCREEN_REC_MIN_MB) {
        msg_update_text_fmt("SD card full");
        return false;
    }

    if (!shadow) {
        shadow = calloc(1, W * H * 4);
        for (int i = 0; i < QUEUE; i++) {
            queue[i].px = malloc(W * H * 4);
        }
        atexit(screen_rec_shutdown);
        label = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_color(label, TC(tx), 0);
        lv_obj_set_style_text_font(label, &sony_24, 0);
        lv_obj_set_style_bg_color(label, TC(panel), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
        lv_obj_set_style_pad_left(label, 26, 0);
        lv_obj_set_style_pad_right(label, 6, 0);
        lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -4, 130 + METER_ROW_H);   /* under the frequency strip (VISION: and the S-meter) */

        lv_obj_t *dot = lv_obj_create(label);       /* the fonts have no bullet */

        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, TC(tx), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, -20, 0);
    }

    if (!shadow || !queue[QUEUE - 1].px) {
        msg_update_text_fmt("Out of memory");
        return false;
    }

    make_path();
    split_frames = split_min[params.screen_rec_split.x % 5] * 60 * SCREEN_REC_FPS;

    q_head = q_tail = q_count = 0;
    ring_r = ring_w = 0;
    error = 0;
    files = 0;
    low_min = -1;
    warned_min = -1;
    shown = -1;
    t0 = now_ms();
    active = true;
    radio_set_rec_moni(true);
    set_state(RUN);

    pthread_t   th;

    if (pthread_create(&th, NULL, writer_thread, NULL) != 0) {
        active = false;
        radio_set_rec_moni(false);
        set_state(IDLE);
        msg_update_text_fmt("Screen rec error");
        return false;
    }
    pthread_detach(th);

    lv_label_set_text(label, "REC 00:00");
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_scr_act());                /* redraw everything into the copy */
    lv_obj_invalidate(lv_layer_top());
    timer = lv_timer_create(timer_cb, 1000 / SCREEN_REC_FPS, NULL);
    msg_update_text_fmt("Screen rec on");
    return true;
}

bool screen_rec_is_on() {
    return active;
}

void screen_rec_toggle() {
    if (active) {
        active = false;
        radio_set_rec_moni(false);
        request_stop();
    } else {
        start();
    }
    buttons_screen_rec_update();
}

/* App exit, power off: stop and wait for the file to be closed */
void screen_rec_shutdown() {
    request_stop();

    for (int i = 0; i < 300 && get_state() != IDLE; i++) {
        usleep(10000);
    }
}
