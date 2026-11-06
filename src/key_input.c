/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The input group every knob and button feeds, and the USB (or Bluetooth) keyboard:
 * found at start and again whenever a USB device comes or goes. CapsLock is its hyper key
 * (kbd_hyper.h): CapsLock+L logs a QSO, CapsLock+Space is PTT, and the others. */

#include "key_input.h"

#include "cw_encoder.h"
#include "cw_field.h"
#include "input_caps.h"
#include "kbd_hyper.h"
#include "kbd_keymap.h"
#include "kbd_rollover.h"
#include "paddle_keyer.h"
#include "pubsub_ids.h"
#include "screen_light.h"
#include "screen_main.h"
#include "usb_devices.h"

#include "lv_drivers/indev/evdev.h"
#include "lv_drivers/indev/xkb.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
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
static kbd_hyper_t    hyper;        /* CapsLock held for a chord */
static bool           dead;         /* the open keyboard's node reads ENODEV: unplugged, or its Bluetooth link gone */
static bool           caps;         /* Caps Lock on, as xkb has it: only a CapsLock tapped alone turns it */
static bool           woke_hold;    /* the press of the CapsLock held woke the screen: its first chord only did that */
static int            swallowed = -1;   /* the key whose press ended the screensaver: its repeats and release too */

/* The keyboard's Caps Lock light to ours. The light is the kernel console's (/dev/tty0), which turns
 * it at every CapsLock press, also one held for a chord: put back at the chord, and set at each
 * keyboard opened (its xkb starts with Caps Lock off) */
static void caps_light(void) {
    static int    fd = -2;
    unsigned char leds;

    if (fd == -2) {
        fd = open("/dev/tty0", O_RDONLY | O_NOCTTY | O_CLOEXEC);
    }
    if (fd >= 0 && ioctl(fd, KDGKBLED, &leds) == 0 && !(leds & K_CAPSLOCK) != !caps) {
        ioctl(fd, KDSKBLED, (unsigned long)(caps ? leds | K_CAPSLOCK : leds & ~K_CAPSLOCK));
    }
}

/* A chord's action, after the read of its key: it opens windows and moves the keyboard's focus */
static void chord_run(void *arg) {
    main_screen_chord((int)(intptr_t)arg);
}

void keyboard_chord(int action) {
    if (action != HYPER_NONE) {
        lv_async_call(chord_run, (void *)(intptr_t)action);
    }
}

/* A CapsLock tap: Caps Lock turns in xkb, and its light */
static void caps_turn(void) {
    xkb_process_key(KEY_CAPSLOCK, 1);
    xkb_process_key(KEY_CAPSLOCK, 0);
    caps = !caps;
    caps_light();
}

/* CapsLock as the hyper key: false when the event goes no further. woke: this press woke the screen */
static bool hyper_key(const struct input_event *ev, bool woke) {
    kbd_hyper_action_t action = HYPER_NONE;

    if (ev->code == KEY_CAPSLOCK && ev->value == 1) {
        woke_hold = woke;
    }
    switch (kbd_hyper_key(&hyper, ev->code, ev->value, &action)) {
        case HYPER_PASS:
            return true;
        case HYPER_CAPS:            /* tapped alone: Caps Lock, as before */
            caps_turn();
            return false;
        case HYPER_CAPS_PASS:       /* the tap, then the key typed over it: in that order */
            caps_turn();
            return true;
        case HYPER_CHORD:
            caps_light();
            /* A chord on a dark screen only lights it, as a front key's press: CapsLock+Space keyed unseen, no TX
             * on the screen */
            if (woke || woke_hold) {
                woke_hold = false;
            } else {
                keyboard_chord(action);
            }
            return false;
        default:
            return false;
    }
}

/* The keyboard open went away: unplugged, its Bluetooth link lost, another one in its place. The releases of its keys
 * never come: the key LVGL holds is let go, or LVGL repeats it into the screen (a frequency step, the volume, a letter
 * keyed in CW Live) until a keyboard opens again; LVGL releases the key it holds. A PTT its CapsLock+Space keyed lets
 * go too */
static void keyboard_gone(void) {
    kbd_keys.down   = false;
    kbd_keys.queued = false;
    main_screen_keyboard_gone();
}

/* The kernel's queue of the keyboard's events overflowed (64 of them, a second of one key's repeat while the GUI is
 * busy) and the events in it are gone, a release among them maybe: CapsLock's left it held for good (every key a
 * chord, Space PTT on the main screen), LVGL's key's left it pressed. The keys down are read back: what went up
 * meanwhile is let go. True with LVGL's key's release in *scancode, *key and *state */
static bool resync(uint16_t *scancode, uint32_t *key, int *state) {
    unsigned long down[KBD_HYPER_LONGS] = {0};     /* a failed read: all up, no hold and no key held */

    ioctl(evdev_fd, EVIOCGKEY(sizeof(down)), down);
    kbd_hyper_sync(&hyper, down);
    if (!kbd_keys.down || BIT_SET(down, kbd_keys.code)) {
        return false;
    }
    *scancode = kbd_keys.code;
    *key      = kbd_keys.key;
    *state    = 0;
    return true;
}

/* Key events of the keyboard, the character from xkb (layout and Shift) */
static bool next_key(void *unused, uint16_t *scancode, uint32_t *key, int *state) {
    struct input_event ev;
    ssize_t            n;

    while ((n = read(evdev_fd, &ev, sizeof(ev))) > 0) {
        if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
            if (resync(scancode, key, state)) {
                return true;
            }
            continue;
        }
        if (ev.type != EV_KEY) {
            continue;
        }
        /* A key at the radio, as the front keys and the knobs: the screen stays lit, a dark one wakes */
        bool saver = backlight_saver_on();
        bool woke  = backlight_tick() && ev.value == 1;

        /* In CW [ ] and the paddle keys are the paddle (paddle_keyer.c): a press stops a
         * message on air, as a break */
        if (paddle_keyer_key(ev.code)) {
            if (ev.value == 1 && cw_encoder_state() != CW_ENCODER_IDLE && !cw_field_key(LV_KEY_ESC)) {
                cw_encoder_stop();
            }
            continue;
        }
        if (!hyper_key(&ev, woke)) {
            continue;
        }
        if (woke && saver) {
            swallowed = ev.code;
            continue;   /* the key that ends the screensaver only ends it: nothing typed (on a dark screen it types) */
        }
        /* Its repeats and its release go no further either: LVGL, which never had its press, took a repeat for one.
         * Pressed again, its release was lost (SYN_DROPPED): that press types */
        if (ev.code == swallowed) {
            swallowed = ev.value == 2 ? swallowed : -1;
            if (ev.value != 1) {
                continue;
            }
        }
        *key      = xkb_process_key(ev.code, ev.value);     /* xkb follows Shift from these too */
        *scancode = ev.code;
        *state    = ev.value;
        if (kbd_keymap(ev.code)) {
            *key = kbd_keymap(ev.code);                     /* xkb gives no character for them */
        }
        /* Scroll Lock and Print Screen pressed away from the main screen: nowhere to go (a text box would type
         * them); let go, they still reach LVGL, which only takes the release of the key it holds */
        if (ev.value != 0 && kbd_keymap_main_only(ev.code) && !main_screen_has_keys()) {
            continue;
        }
        return true;
    }
    if (n < 0 && errno == ENODEV && !dead) {
        dead = true;
        keyboard_gone();
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
    if (!ok || !BIT_SET(bits, KEY_SPACE)) {
        return false;
    }
    /* Letters: once opened, to be sure it can be (busy, going away), or it would replace the keyboard in use
     * with one that reads nothing. Only keyboards get this far: a mouse is still never opened (#79) */
    int fd = open(path, O_RDONLY | O_NONBLOCK);

    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
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
    hyper    = (kbd_hyper_t){0};
    dead     = false;
    caps     = false;
    caps_light();
    paddle_keyer_open(path);

    if (kbd_drv.type == LV_INDEV_TYPE_NONE) {
        lv_indev_drv_init(&kbd_drv);
        kbd_drv.type    = LV_INDEV_TYPE_KEYPAD;
        kbd_drv.read_cb = kbd_read;
        lv_indev_set_group(lv_indev_drv_register(&kbd_drv), keyboard_group);
    }
    ready = true;
}

/* A keyboard there and not open (or a different one: unplugged and back): open it; none: not ready. The one open
 * gone, or another in its place: keyboard_gone() */
static void check_keyboard(void) {
    char       *path = find_keyboard();
    struct stat st;

    if (path && stat(path, &st) == 0) {
        if (!ready || st.st_rdev != kbd_rdev || st.st_ino != kbd_ino) {
            if (ready) {
                keyboard_gone();
            }
            open_keyboard(path);
        }
    } else {
        if (ready) {
            keyboard_gone();
        }
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
