/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The input group every knob and button feeds, and the USB (or Bluetooth) keyboard:
 * found at start and again whenever a USB device comes or goes. */

#include "key_input.h"

#include "cw_encoder.h"
#include "cw_field.h"
#include "input_caps.h"
#include "kbd_rollover.h"
#include "paddle_keyer.h"
#include "pubsub_ids.h"
#include "usb_devices.h"

#include "lv_drivers/indev/evdev.h"
#include "lv_drivers/indev/xkb.h"

#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* After a USB device comes or goes, look for the keyboard this often, this many times: udev's
 * "add" comes before the keyboard's /dev/input node exists */
#define RESCAN_MS    500
#define RESCAN_TIMES 10

#define BIT_SET(bits, n) ((bits)[(n) / (8 * sizeof(long))] & (1UL << ((n) % (8 * sizeof(long)))))

extern int evdev_fd;    /* the keyboard opened by evdev_set_file() (lv_drivers) */

lv_group_t *keyboard_group;

static lv_indev_drv_t kbd_drv;      /* registered with the first keyboard, kept after */
static bool           ready;
static kbd_keys_t     kbd_keys;     /* what LVGL sees of the keyboard */
static dev_t          kbd_rdev;     /* the one open: a different node (unplugged and back) opens again */
static ino_t          kbd_ino;
static lv_timer_t    *rescan_timer;
static int            rescans_left;

/* Key events of the keyboard, the character from xkb (layout and Shift) */
static bool next_key(void *unused, uint16_t *scancode, uint32_t *key, int *state) {
    struct input_event ev;

    while (read(evdev_fd, &ev, sizeof(ev)) > 0) {
        if (ev.type != EV_KEY) {
            continue;
        }
        /* In CW [ ] and the paddle keys are the paddle (paddle_keyer.c): a press stops a
         * message on air, as a break */
        if (paddle_keyer_key(ev.code)) {
            if (ev.value == 1 && cw_encoder_state() != CW_ENCODER_IDLE && !cw_field_key(LV_KEY_ESC)) {
                cw_encoder_stop();
            }
            continue;
        }
        *key      = xkb_process_key(ev.code, ev.value);     /* xkb follows Shift from these too */
        *scancode = ev.code;
        *state    = ev.value;
        if (ev.code >= KEY_F1 && ev.code <= KEY_F10) {
            *key = KEYBOARD_F1 + ev.code - KEY_F1;          /* xkb gives no character for them */
        }
        return true;
    }
    return false;
}

static void kbd_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    kbd_keys_read(&kbd_keys, next_key, NULL, data);
}

/* A device with the keys of a keyboard (the top letter row and space): the radio's own
 * buttons and knobs have none of these. Read from sysfs: opening each device to look stalled
 * the GUI 50 ms for every node of a USB mouse, on every scan after a plug (#79, input_caps.h) */
static bool has_letters(const char *path) {
    unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1];
    struct stat   st;
    bool          ok = stat(path, &st) == 0 &&
                       input_caps_bits(st.st_rdev, "key", bits, sizeof(bits) / sizeof(bits[0]));

    for (int k = KEY_Q; ok && k <= KEY_P; k++) {
        ok = BIT_SET(bits, k);
    }
    return ok && BIT_SET(bits, KEY_SPACE);
}

/* The first device matching pattern that has letters, into path */
static bool find_letters(const char *pattern, char *path, size_t size) {
    glob_t found;
    bool   ok = false;

    if (glob(pattern, 0, NULL, &found) != 0) {
        return false;
    }
    for (size_t i = 0; i < found.gl_pathc && !ok; i++) {
        if (has_letters(found.gl_pathv[i])) {
            snprintf(path, size, "%s", found.gl_pathv[i]);
            ok = true;
        }
    }
    globfree(&found);
    return ok;
}

/* USB keyboards by path (a wireless receiver or one with media keys has several "-kbd"
 * links: the one with the letters); Bluetooth ones have no path link: by their keys */
static char *find_keyboard(void) {
    static char path[256];      /* a by-path link behind a hub is longer than 64 */

    if (find_letters("/dev/input/by-path/*-kbd", path, sizeof(path)) ||
        find_letters("/dev/input/event*", path, sizeof(path))) {
        return path;
    }
    return NULL;
}

static void open_keyboard(char *path) {
    struct stat st;

    if (stat(path, &st) != 0 || !evdev_set_file(path)) {
        return;
    }
    kbd_rdev = st.st_rdev;
    kbd_ino  = st.st_ino;
    kbd_keys = (kbd_keys_t){0};
    paddle_keyer_open(path);

    if (kbd_drv.type == LV_INDEV_TYPE_NONE) {
        lv_indev_drv_init(&kbd_drv);
        kbd_drv.type    = LV_INDEV_TYPE_KEYPAD;
        kbd_drv.read_cb = kbd_read;
        lv_indev_set_group(lv_indev_drv_register(&kbd_drv), keyboard_group);
    }
    ready = true;
}

/* A keyboard there and not open (or a different one: unplugged and back): open it; none: not ready */
static void check_keyboard(void) {
    char       *path = find_keyboard();
    struct stat st;

    if (path && stat(path, &st) == 0) {
        if (!ready || st.st_rdev != kbd_rdev || st.st_ino != kbd_ino) {
            open_keyboard(path);
        }
    } else {
        ready = false;
    }
    paddle_keyer_scan();   /* after the keyboard: a second one with the paddle keys is the paddle */
}

/* Every time, also once one is found: an unplugged keyboard's link can outlive the "remove" */
static void rescan_cb(lv_timer_t *t) {
    check_keyboard();
    if (--rescans_left <= 0) {
        lv_timer_del(t);
        rescan_timer = NULL;
    }
}

static void usb_change_cb(void *s, lv_msg_t *msg) {
    check_keyboard();
    rescans_left = RESCAN_TIMES;
    if (!rescan_timer) {
        rescan_timer = lv_timer_create(rescan_cb, RESCAN_MS, NULL);
    }
}

void keyboard_init() {
    keyboard_group = lv_group_create();
    /* The MFK stops at the first and last item of a page instead of jumping round */
    lv_group_set_wrap(keyboard_group, false);
    lv_msg_subscribe(MSG_USB_DEVICE_CHANGED, usb_change_cb, NULL);

    char *path = find_keyboard();

    if (path) {
        open_keyboard(path);
    }
}

bool keyboard_ready() {
    return ready;
}
