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

#include "dialog_field.h"
#include "msg_box.h"
#include "board.h"
#include "lvgl/lvgl.h"
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
#include "fonts/font_select.h"
#include "radio.h"
#include "dsp.h"
#include "util.h"
#include "key_input.h"
#include "paddle_keyer.h"
#include "front_keys.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "audio.h"
#include "cw.h"
#include "cat.h"
#include "tci_server.h"
#include "remote_server.h"
#include "control_owner.h"
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
 * must exist and the legacy spectrum values must stay in their ranges. */
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
}

#if USE_DRM
/* SIGUSR1: the screen to /tmp/screen.raw (with DRM /dev/fb0 no longer shows it) */
static volatile sig_atomic_t dump_requested;

static void dump_signal(int sig) {
    dump_requested = 1;
}
#endif

/* The panel is portrait: the walkie screen goes to it turned 180, not 90. DRM only */
bool main_display_portrait(bool on) {
#if USE_DRM
    if (screen_rec_out != drm_flush) {
        return false;
    }
    drm_set_portrait(on);
    disp_drv.hor_res = on ? BOARD_DISP_H : BOARD_DISP_W;
    disp_drv.ver_res = on ? BOARD_DISP_W : BOARD_DISP_H;
    lv_disp_drv_update(lv_disp_get_default(), &disp_drv);
    return true;
#else
    return false;
#endif
}

/* The panel: DRM planes (the GUI over the waterfall) when the kernel has them, the old
 * framebuffer otherwise */
static void panel_open(void) {
#if USE_SDL
    host_panel_open();
    screen_rec_out = host_panel_flush;
    return;
#endif
#if USE_DRM
    signal(SIGUSR1, dump_signal);
    if (drm_init()) {
        screen_rec_out = drm_flush;
        fclose(fopen("/tmp/x6100_gui.drm", "w"));  /* tells screenshot tools to use SIGUSR1 */
        return;
    }
    unlink("/tmp/x6100_gui.drm");
#endif
#if USE_FBDEV
    fbdev_init();
#endif
}

/* LVGL's display, 800x480 landscape; every frame goes through the screen recorder */
static void display_register(void) {
    lv_disp_t *disp;

    lv_disp_draw_buf_init(&draw_buf, draw_mem, NULL, DISP_BUF_SIZE);
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = screen_rec_flush;
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
    keypad_init(BOARD_DEV_KEYPAD);
    keypad_init(BOARD_DEV_POWER);
    rotary_main_init(BOARD_DEV_MAIN);

    vol = rotary_init(BOARD_DEV_VOL);
    mfk = encoder_init(BOARD_DEV_MFK);

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
    /* Bundled themes to the SD card once; one already there is never overwritten */
    if (system("mkdir -p " BOARD_DATA "/themes; for t in " BOARD_SHARE "/themes/*; do "
               "[ -e \"" BOARD_DATA "/themes/${t##*/}\" ] || cp -r \"$t\" " BOARD_DATA "/themes/; done") != 0)
        LV_LOG_WARN("Bundled themes not copied to " BOARD_DATA "/themes");
    styles_load(params.theme_name.x);
}

/* Everything that runs beside the GUI, once the radio is up */
static void services_start(void) {
    cw_init();
    rtty_init();
    radio_set_rx_tx_notify_fn(&main_screen_notify_rx_tx);
    radio_set_low_power_cb(&main_screen_notify_low_power);
    wifi_power_setup();
    btaudio_init();
    bt_dev_init();
    backlight_init();
    cat_init();
    cfg_socket_init();
    ctl_init();
    tci_server_init();
    remote_server_init();
    winlink_init();     /* ardopcf's audio and Pat's rigctld: idle until they connect */
    birdie_init();
    gps_init();
    if (!qso_log_init()) {
        LV_LOG_ERROR("Can't init QSO log");
    }
    qso_log_import_adif(BOARD_DATA "/incoming_log.adi");
    dialog_cluster_start_client();
    image_level_notice_init();   /* a newer card image wanted: said once per version */
}

/* LVGL's clock: 5 ms ticks */
static void *tick_thread(void *arg) {
    for (;;) {
        usleep(5000);
        lv_tick_inc(5);
    }
    return NULL;
}

static void tick_start(void) {
    pthread_t thread;

    pthread_create(&thread, NULL, tick_thread, NULL);
    pthread_detach(thread);
}

/* The events from the threads, the scheduled work, LVGL; then sleep until LVGL wants to
 * run again, at least 5 ms (when it says now) to keep the CPU free */
/* A version installed by x6100-update is "pending" until the GUI has run a minute: then it is
 * confirmed, otherwise the start loop rolls it back after three failed starts */
static void update_confirm_cb(lv_timer_t *t) {
    sh_run("/usr/sbin/x6100-update confirm", NULL, NULL);
}

static void gui_loop(void) {
    for (;;) {
        int64_t start = get_time();

        event_obj_check();
        scheduler_work();

        int64_t next = start + lv_timer_handler();
#if USE_DRM
        drm_commit();
        if (dump_requested) {
            dump_requested = 0;
            drm_dump("/tmp/screen.raw.tmp");
            rename("/tmp/screen.raw.tmp", "/tmp/screen.raw");
        }
#endif
        int64_t wait = next - get_time();

        usleep(wait > 0 ? wait * 1000 : 5000);
    }
}

int main(void) {
    scheduler_init();   /* this is the GUI thread */
    setenv("X6100_FW", strrchr(VERSION, 'v') + 1, 1);   /* the helpers' User-Agent (x6100-field, spots) */
    clock_tz_init();
    lv_init();
    panel_open();
    audio_init();
    event_init();
    usb_devices_monitor_init();
    display_register();
    inputs_open();
    settings_load();

    radio_init();
    audio_mixer_setup(radio_base_ver());
    dsp_init();
    lv_obj_t *screen = main_screen();
    radio_start();
    services_start();
    tick_start();

    lv_scr_load(screen);
    walkie_init(screen);      /* its screen instead, when on */
    if (!walkie_on() && param_i_get(cfg_field_boot)) {
        main_screen_start_app(ACTION_APP_FIELD);
    }
    lv_timer_set_repeat_count(lv_timer_create(update_confirm_cb, 60000, NULL), 1);
    dialog_field_wavelog_start();
    gui_loop();
    return 0;
}
