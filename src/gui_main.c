/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 *
 *  Start: the screen, the input devices, the settings, the radio and the services in the
 *  order they need each other, then the GUI loop.
 */

#include "gui_wake.h"
#include "dialog_field.h"
#include "msg_box.h"
#include "board.h"
#include "lvgl/lvgl.h"
#include "lv_tick_mono.h"
#if USE_SDL
#include "host/host_panel.h"
#else
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/display/drm.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include "main.h"
#include "screen_main.h"
#include "screen_rec.h"
#include "gui_styles.h"
#include "mono.h"
#include "fonts/font_select.h"
#include "radio.h"
#include "dsp.h"
#include "util.h"
#include "key_input.h"
#include "paddle_keyer.h"
#include "front_keys.h"
#include "params/params.h"
#include "params/db.h"
#include "cfg/cfg_api.h"
#include "audio.h"
#include "cw.h"
#include "cat.h"
#include "tci_server.h"
#include "remote_server.h"
#include "control_owner.h"
#include "ext_tune.h"
#include "birdie.h"
#include "cfg_socket.h"
#include "clock.h"
#include "btaudio.h"
#include "rtty.h"
#include "screen_light.h"
#include "gui_events.h"
#include "gps.h"
#include "mfk_control.h"
#include "vol_control.h"
#include "qso_log.h"
#include "scheduler.h"
#include "wifi.h"
#include "btaudio.h"
#include "bt_dev.h"
#include "usb_devices.h"
#include "image_level.h"
#include "start_trace.h"
#include "dialog_cluster.h"
#include "app_sstv.h"
#include "walkie.h"
#include "winlink.h"

/* LVGL draws the areas it refreshes here, then flushes them to the panel */
#define DISP_BUF_SIZE (BOARD_DISP_W * BOARD_DISP_H * 4)

rotary_t                    *vol;
encoder_t                   *mfk;

static lv_color_t           draw_mem[DISP_BUF_SIZE];
static lv_disp_draw_buf_t   draw_buf;
static lv_disp_drv_t        disp_drv;

#if USE_DRM
/* Transparent screen: LVGL cleared the whole draw buffer (6 MB) before every area it redraws,
 * even the clock's seconds, ~30 times a second. The area is drawn packed: clear just that. */
static void clear_cb(lv_disp_drv_t *drv, uint8_t *buf, uint32_t size) {
    memset(buf, 0, lv_area_get_size(drv->draw_ctx->buf_area) * sizeof(lv_color_t));
}
#endif

/* Display settings that would break the screen are put right at every start
 * (an old or hand-edited params.db, a backup restored from another version):
 * the spectrum is disabled in this firmware, the font
 * must exist and the legacy spectrum values and the screensaver must stay in their ranges. */
static void fix_uint8(params_uint8_t *p) {
    if (p->max > p->min && (p->x < p->min || p->x > p->max)) {
        uint8_t v = p->x < p->min ? p->min : p->max;

        fprintf(stderr, "Settings: %s %u out of range, set to %u\n", p->name, p->x, v);
        params_uint8_set(p, v);
    }
}

static void display_settings_check(void) {
    if (params.font.x > FONT_DEJAVU) {
        fprintf(stderr, "Settings: font %u unknown, set to the default\n", params.font.x);
        params_uint8_set(&params.font, FONT_SONY);
    }
    fix_uint8(&params.spectrum_beta);
    fix_uint8(&params.spectrum_peak_hold);
    fix_uint8(&params.spectrum_peak_speed);
    fix_uint8(&params.screen_saver);
    fix_uint8(&params.screen_saver_speed);
}

#if USE_DRM || USE_SDL
/* SIGUSR1: the screen to /tmp/screen.raw (with DRM /dev/fb0 no longer shows it; on a PC the
 * window's last frame, for tools/screenshots/host_shots.py) */
static volatile sig_atomic_t dump_requested;

static void dump_signal(int sig) {
    dump_requested = 1;
}
#endif

/* The panel is portrait: the walkie screen goes to it turned 180, not 90. DRM only,
 * and only when the framebuffer is the radios' portrait layout. */
bool main_display_portrait(bool on) {
#if USE_DRM && BOARD_FB_ROTATE_90
    if (screen_rec_out != drm_flush) {
        return false;
    }
    drm_set_portrait(on);
    disp_drv.hor_res = on ? BOARD_DISP_H : BOARD_DISP_W;
    disp_drv.ver_res = on ? BOARD_DISP_W : BOARD_DISP_H;
    lv_disp_drv_update(lv_disp_get_default(), &disp_drv);
    return true;
#else
    (void)on;
    return false;
#endif
}

/* The panel: DRM planes (the GUI over the waterfall) when the kernel has them, the old
 * framebuffer otherwise */
static void panel_open(void) {
#if USE_SDL
    host_panel_open();
    screen_rec_out = host_panel_flush;
    signal(SIGUSR1, dump_signal);
    return;
#endif
#if USE_DRM
    signal(SIGUSR1, dump_signal);
    if (drm_init()) {
        screen_rec_out = drm_flush;
        fclose(fopen("/tmp/x6100_gui.drm", "w"));  /* the GUI owns the screen: the boot animation stops */
        return;
    }
    unlink("/tmp/x6100_gui.drm");
#endif
#if USE_FBDEV
    fbdev_init();
    /* As with DRM: the boot animation sees the GUI is up and stops drawing into fb0 (it draws only on a portrait
     * one). No screenshot tool reads it, and SIGUSR1's dump to /tmp/screen.raw has nothing to write on fbdev */
    fclose(fopen("/tmp/x6100_gui.drm", "w"));
#endif
}

/* LVGL's display, 800x480 landscape; every frame goes through the screen recorder */
static void display_register(void) {
    lv_disp_t *disp;

    lv_disp_draw_buf_init(&draw_buf, draw_mem, NULL, DISP_BUF_SIZE);
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = screen_rec_flush;
    disp_drv.draw_ctx_init = mono_draw_ctx_init;   /* LVGL's software renderer; 1bit hooks into it */
    disp_drv.hor_res  = BOARD_DISP_W;
    disp_drv.ver_res  = BOARD_DISP_H;
#if USE_DRM
    /* GUI over the waterfall plane: see-through parts keep their alpha */
    disp_drv.screen_transp = drm_planes();
    disp_drv.clear_cb = clear_cb;
#endif
    lv_disp_drv_register(&disp_drv);

    disp = lv_disp_get_default();
    lv_disp_set_bg_color(disp, lv_color_black());
    lv_disp_set_bg_opa(disp, LV_OPA_COVER);
}

/* Front keys (event0, the POWER key on its own device where the board has one), main knob,
 * VOL, MFK (board.h) */
static void inputs_open(void) {
#if USE_SDL
    host_panel_inputs();    /* the PC's keyboard, instead of a USB one found by itself */
#else
    keyboard_init();
#endif
    keypad_t *keys = keypad_init(BOARD_DEV_KEYPAD), *power = keypad_init(BOARD_DEV_POWER);

    rotary_main_init(BOARD_DEV_MAIN);   /* its thread wakes the GUI itself */

    vol = rotary_init(BOARD_DEV_VOL);
    mfk = encoder_init(BOARD_DEV_MFK);

    /* A key or a knob wakes the GUI loop at once (gui_wake.h) */
    if (keys) gui_wake_add_fd(keys->fd);
    if (power) gui_wake_add_fd(power->fd);
    if (vol) gui_wake_add_fd(vol->fd);
    if (mfk) gui_wake_add_fd(mfk->fd);

    /* VOL turns are keys to the focused widget: the value, or the choice of what it controls */
    vol->left[VOL_STATE_EDIT]    = KEY_VOL_LEFT_EDIT;
    vol->right[VOL_STATE_EDIT]   = KEY_VOL_RIGHT_EDIT;
    vol->left[VOL_STATE_SELECT]  = KEY_VOL_LEFT_SELECT;
    vol->right[VOL_STATE_SELECT] = KEY_VOL_RIGHT_SELECT;
    vol->state                   = VOL_STATE_EDIT;

#ifdef BOARD_DEV_MFK_INNER
    /* The inner ring of the MFK knob chooses what the MFK controls, in every state (the stock
     * X6200 GUI's use of it); the outer knob then changes the value */
    rotary_t *inner = rotary_init(BOARD_DEV_MFK_INNER);

    if (inner) {
        gui_wake_add_fd(inner->fd);
        for (int i = 0; i < 3; i++) {
            inner->left[i]  = KEY_MFK_PREV;
            inner->right[i] = KEY_MFK_NEXT;
        }
    }
#endif
}

/* The settings, and what is made from them before the screen is built */
static void settings_load(void) {
    params_init();
    paddle_keyer_scan();    /* the paddle maps are settings */
    sstv_install_files();
    display_settings_check();
    audio_set_play_vol(params.play_gain_db_f.x);
    audio_set_rec_vol(params.rec_gain_db_f.x);
    mfk_init();
    vol_init();
#if !USE_SDL
    keyboard_layout_start();    /* the keyboard opened before the settings were read: its layout now */
#endif
    /* The folder for the user's own themes (docs/THEMES.md). None comes with the image any more: the RS
     * theme that was copied here is cut from a screenshot of another maker's remote software (themes/make_rs.py) */
    if (system("mkdir -p " BOARD_DATA "/themes") != 0)
        LV_LOG_WARN("No " BOARD_DATA "/themes folder");
    styles_load(params.theme_name.x);
}

/* Settings that did not come from params.db as it was (params/db.c: one the GUI could not use, the last good copy or
 * the defaults in its place), said once the screen is up; long enough for the line to scroll through */
static void settings_note_cb(lv_timer_t *t) {
    const char *note = database_note();

    (void)t;
    if (note) {
        msg_schedule_text_ms(8000, "%s", note);
    }
}

/* A minute up on settings that loaded: the last good copy of params.db (params/db.c), what a damaged one is replaced
 * with at a start, made in a thread of its own. The update's confirm comes at the same moment */
static void settings_keep_cb(lv_timer_t *t) {
    (void)t;
    database_keep_later();
}

#define UPDATE_CTL "/usr/sbin/x6100-update"

/* A version installed by x6100-update is "pending" until the GUI has run a minute: then it is
 * confirmed, otherwise the start loop rolls it back after three failed starts */
static void update_confirm_cb(lv_timer_t *t) {
    sh_run(UPDATE_CTL " confirm", NULL, NULL);
}

/* A version taken back, said once at the start after it, which version, to which and why: it did not stay up, it
 * could not use the settings file, by hand, or its install was cut short. The step back happens before this GUI
 * starts (x6100-update boot-check, or the web page's button), so nothing else on the screen would tell */
static void update_notice_done(const char *out, int status, void *user) {
    (void)user;
    if (status == 0 && strncmp(out, "ok ", 3) == 0 && out[3]) {
        msg_schedule_text_ms(8000, "%.*s", (int)strcspn(out + 3, "\r\n"), out + 3);
    }
}

static void update_notice_cb(lv_timer_t *t) {
    (void)t;
    sh_run(UPDATE_CTL " notice", update_notice_done, NULL);
}

/* Everything that runs beside the GUI, once the radio is up */
static void services_start(void) {
    cw_init();
    rtty_init();
    radio_set_rx_tx_notify_fn(&main_screen_notify_rx_tx);
    radio_set_low_power_cb(&main_screen_notify_low_power);
    /* The start trace after each step that talks to another program or the card: a hang shows between two lines */
    wifi_power_setup();     /* NetworkManager, its calls waited for */
    start_trace("services: WiFi set up");
    btaudio_init();
    bt_dev_init();
    start_trace("services: Bluetooth set up");
    backlight_init();
    cat_init();
    cfg_socket_init();
    ctl_init();
    ext_tune_init();    /* TUNE for an external tuner: stopped when the radio moves */
    tci_server_init();
    remote_server_init();
    winlink_init();     /* ardopcf's audio and Pat's rigctld: idle until they connect */
    start_trace("services: CAT, TCI and the browser's servers up");
    birdie_init();
    gps_init();
    if (!qso_log_init()) {
        LV_LOG_ERROR("Can't init QSO log");
    }
    qso_log_import_adif(BOARD_DATA "/incoming_log.adi");
    start_trace("services: QSO log read");
    dialog_cluster_start_client();
    image_level_notice_init();   /* a newer card image wanted: said once per version */
    lv_timer_set_repeat_count(lv_timer_create(settings_note_cb, 3000, NULL), 1);
    lv_timer_set_repeat_count(lv_timer_create(update_notice_cb, 4000, NULL), 1);
}

/* LVGL's clock (lv_tick_mono.h): the GUI loop sets it before each lv_timer_handler(), this thread every 5 ms
 * meanwhile, through a long frame and for the threads that read it. The kernel's monotonic clock, as get_time() */
uint32_t lv_tick_mono_now;

static void *tick_thread(void *arg) {
    for (;;) {
        usleep(5000);
        lv_tick_mono_read();
    }
    return NULL;
}

static void tick_start(void) {
    pthread_t thread;

    /* The first as it is, before both writers (lv_tick_mono.h) */
    __atomic_store_n(&lv_tick_mono_now, (uint32_t)get_time(), __ATOMIC_RELAXED);
    pthread_create(&thread, NULL, tick_thread, NULL);
    pthread_detach(thread);
}

/* The events from the threads, the scheduled work, LVGL; then sleep until LVGL wants to
 * run again, or until a key, a knob or another thread wakes it (gui_wake.h) */

/* Input drawn at once, frames at least this far apart. On the radios one frame of the panel: drm_flush() waits for
 * the flip of the frame before, so a frame drawn sooner is not shown sooner, and the GUI thread waits in wait_flip()
 * meanwhile (the scheduler's jobs and the events with it). The PC build 8 ms */
static uint32_t min_frame_gap(void) {
#if USE_DRM
    return drm_frame_ms();
#else
    return 8;
#endif
}

/* When the screen's refresh timer last ran: it ran in a lv_timer_handler() when this changed */
static uint32_t refr_last_run(void) {
    lv_disp_t *d = lv_disp_get_default();

    return d && d->refr_timer ? d->refr_timer->last_run : 0;
}

static void gui_loop(void) {
    bool drawn = false;     /* the start trace's last stage */

    for (;;) {
        event_obj_check();
        scheduler_work();
        lv_tick_mono_set(get_time());   /* LVGL's timers on the clock's time, not up to 5 ms behind */
        gui_wake_draw_due();
        uint32_t frame   = refr_last_run();
        uint32_t till    = lv_timer_handler();
        /* LVGL counts the time to its next timer from its clock at the end of the run, not from the loop's top:
         * from there the loop woke early after every long frame, to sleep again */
        uint32_t due     = lv_tick_mono_ms() + till;
        bool     drew    = refr_last_run() != frame;
#if USE_DRM
        drm_commit();
        if (dump_requested) {
            dump_requested = 0;
            drm_dump("/tmp/screen.raw.tmp");
            rename("/tmp/screen.raw.tmp", "/tmp/screen.raw");
        }
#endif
#if USE_SDL
        if (dump_requested) {
            dump_requested = 0;
            if (host_panel_dump("/tmp/screen.raw.tmp")) rename("/tmp/screen.raw.tmp", "/tmp/screen.raw");
        }
#endif
        if (!drawn) {
            drawn = true;
            start_trace("first frame");
        }
        /* Both counted from after the commit: a put-off draw whose time came during the pass after the least wait */
        uint32_t now     = (uint32_t)get_time();
        int32_t  wait    = till == LV_NO_TIMER_READY ? 100 : (int32_t)(due - now);
        int32_t  draw_in = gui_wake_draw_left(now);

        if (draw_in >= 0 && draw_in < wait) wait = draw_in;
        /* A timer due at once after a frame (a frame that ran past its period, 3D with FT8 on the X6100): 5 ms for
         * the other threads, as before #183; else 1 ms. Input wakes it all the same */
        int32_t least = drew && till == 0 ? 5 : 1;

        /* Until LVGL's next timer (at most 100 ms), or input: then the keys and knobs are read at once, and what
         * they changed drawn, frames at least min_frame_gap() apart */
        if (gui_wake_wait(wait < least ? least : wait > 100 ? 100 : wait) == WAKE_INPUT) {
            lv_tick_mono_set(get_time());
            gui_wake_read_input();
            frame = refr_last_run();
            lv_timer_handler();
            if (refr_last_run() != frame) {
                /* The refresh was due: it ran after the reads (LVGL runs its newer timers first, the indevs'
                 * after the display's), what they changed is drawn. Shown now, not after the next pass's events
                 * and jobs */
#if USE_DRM
                drm_commit();
#endif
            } else {
                gui_wake_draw_now(min_frame_gap());
            }
        }
    }
}

int main(void) {
    tick_start();       /* LVGL's clock, from the start */
    gui_wake_init();    /* the loop's wake, before any thread that posts to it */
    scheduler_init();   /* this is the GUI thread */
    start_trace("GUI start, %s", VERSION);
    /* the helpers' User-Agent (x6100-field, spots), the logs' PROGRAMVERSION (field_adif_header) */
    setenv("X6100_FW", strrchr(VERSION, 'v') + 1, 1);
    clock_tz_init();
    lv_init();
    panel_open();
    audio_init();
    event_init();
    usb_devices_monitor_init();
    display_register();
    inputs_open();
    settings_load();
    start_trace("settings read");

    if (!radio_init()) {
        /* Held by another GUI, or not there: without it nothing here works, and going on crashed on it */
        fprintf(stderr, "base: its pins or its line could not be opened, exiting\n");
        return 1;
    }
    audio_mixer_setup(radio_base_ver());
    dsp_init();
    lv_obj_t *screen = main_screen();
    start_trace("screen built");
    radio_start();
    services_start();
    start_trace("services started");

    lv_scr_load(screen);
    walkie_init(screen);      /* its screen instead, when on */
    if (!walkie_on() && param_i_get(cfg_field_boot)) {
        main_screen_start_app(ACTION_APP_FIELD);
    }
    /* Not when this version could not use the settings file (params/db.c): the update is then taken back, and the
     * updater told why, for what it says after the step back */
    if (!database_version_fault()) {
        lv_timer_set_repeat_count(lv_timer_create(update_confirm_cb, 60000, NULL), 1);
    } else {
        sh_run(UPDATE_CTL " fault settings", NULL, NULL);
    }
    lv_timer_set_repeat_count(lv_timer_create(settings_keep_cb, 60000, NULL), 1);
    dialog_field_wavelog_start();
    gui_loop();
    return 0;
}
