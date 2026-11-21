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
#include "voice_focus.h"

#include "cw_encoder.h"
#include "cw_field.h"
#include "cfg/cfg_api.h"
#include "input_caps.h"
#include "cfg/kbd_devices.h"
#include "kbd_hyper.h"
#include "kbd_keymap.h"
#include "kbd_layout.h"
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
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

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
static bool           layout_on;    /* the settings are there: the keyboard's layout follows Settings > Keys (#236) */
static bool           caps_digits;  /* the layout xkb has types the number row's digits with Caps Lock on (#254) */
static bool           kbd_vol;      /* the keyboard open is set to Volume (kbd_devices.h): [ ] the volume, no paddle */
static char           kbd_path[256];

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

/* The layout of Settings > Keys > KEYBOARD into xkb (#236). evdev_set_file() starts xkb on US at every keyboard
 * opened, so after each, and at each change while one is open (xkb has no context before the first). A layout
 * xkb cannot make goes back to US rather than leave no keymap. The new state starts with Caps Lock off: put
 * back as it was */
static void layout_apply(void) {
    const kbd_layout_t   *l;
    struct xkb_rule_names names;

    if (!layout_on || !ready) {
        return;
    }
    l = kbd_layout_get(param_i_get(cfg_kbd_layout));
    names = (struct xkb_rule_names){.model = l->model, .layout = l->layout};
    caps_digits = l->caps_digits;
    if (!xkb_set_keymap(names)) {
        LV_LOG_ERROR("Keyboard layout %s: xkb has none, US kept", l->layout);
        names = (struct xkb_rule_names){.model = "pc101", .layout = "us"};
        xkb_set_keymap(names);
        caps_digits = false;
    }
    if (caps) {
        xkb_process_key(KEY_CAPSLOCK, 1);
        xkb_process_key(KEY_CAPSLOCK, 0);
    }
}

static void layout_cb(Subject *subj, void *user) {
    (void)subj;
    (void)user;
    layout_apply();
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
static void vol_close(bool kbd_media_only);

static void keyboard_gone(void) {
    kbd_keys.down   = false;
    kbd_keys.queued = false;
    vol_close(true);
    main_screen_keyboard_gone();
}

/* The USB or Bluetooth device of node dev is set to Volume in Settings > Keys > KEYBOARD (kbd_devices.h, #306). Not
 * before the settings are there */
static bool dev_volume(dev_t dev) {
    char            list[KBD_DEV_LIST_LEN], one[KBD_DEV_ID_LEN];
    struct input_id id;

    if (!layout_on || !cfg_kbd_dev_volume || !input_caps_id(dev, &id) ||
        (id.bustype != BUS_USB && id.bustype != BUS_BLUETOOTH)) {
        return false;
    }
    param_t_get(cfg_kbd_dev_volume, list, sizeof(list));
    kbd_dev_id(one, id.bustype, id.vendor, id.product);
    return kbd_dev_listed(list, one);
}

bool paddle_keyer_volume_dev(dev_t dev) {
    return dev_volume(dev);
}

/* The volume keys (kbd_keymap_volume), and [ ] of a device set to Volume (kbd_keymap_brackets), step the RX volume as
 * the H-22's VOL+ and VOL-, in every window and in CW: they never reach LVGL, which would type their keysym into a
 * text box, nor the paddle. A press steps once, the kernel's repeat of a held one every MEDIA_VOL_REPEAT_MS at most. A
 * key that ends the screensaver only ends it */
#define MEDIA_VOL_REPEAT_MS 100

static void media_vol_run(void *arg) {
    main_screen_media_vol((int)(intptr_t)arg);
}

static void media_key(const struct input_event *ev, int dir, bool only_wake) {
    static uint32_t last;

    if (ev->value == 0 || only_wake || (ev->value == 2 && lv_tick_elaps(last) < MEDIA_VOL_REPEAT_MS)) {
        return;
    }
    last = lv_tick_get();
    lv_async_call(media_vol_run, (void *)(intptr_t)dir);
}

/* The direction of a key that steps the volume, 0 for the others: brackets, [ ] too */
static int vol_dir(uint16_t code, bool brackets) {
    int dir = kbd_keymap_volume(code);

    return dir ? dir : brackets ? kbd_keymap_brackets(code) : 0;
}

/* Nodes read beside the keyboard for the volume only, up to VOL_NODES:
 * - the keyboard's media keys on a node of their own ("... Consumer Control", the kernel's hid-generic makes one a HID
 *   application collection), with no letters, which find_keyboard() passes over: the node with Volume Up and Down of
 *   the same bus, vendor and product as the keyboard open;
 * - a USB or Bluetooth device set to Volume that is not the keyboard open (a volume knob beside a keyboard): its [ ]
 *   and volume keys.
 * Their other keys are dropped. A timer of their own reads them, as there may be no keyboard open. Closed when gone
 * (the next scan finds them again), the keyboard's with it, all when the setting changes */
#define VOL_NODES   4
#define VOL_POLL_MS 30

static struct {
    int   fd;
    dev_t rdev;
    bool  brackets;     /* the device is set to Volume: [ ] step the volume too */
    bool  kbd_media;    /* the keyboard's own media node: closed with the keyboard */
} vol_node[VOL_NODES] = {{.fd = -1}, {.fd = -1}, {.fd = -1}, {.fd = -1}};

static lv_timer_t *vol_timer;

static void vol_read_cb(lv_timer_t *t);

static void vol_timer_update(void) {
    bool any = false;

    for (int i = 0; i < VOL_NODES; i++) {
        any |= vol_node[i].fd >= 0;
    }
    if (any && !vol_timer) {
        vol_timer = lv_timer_create(vol_read_cb, VOL_POLL_MS, NULL);
    } else if (!any && vol_timer) {
        lv_timer_del(vol_timer);
        vol_timer = NULL;
    }
}

static void vol_close_one(int i) {
    if (vol_node[i].fd >= 0) {
        close(vol_node[i].fd);
    }
    vol_node[i] = (typeof(vol_node[i])){.fd = -1};
}

static void vol_close(bool kbd_media_only) {
    for (int i = 0; i < VOL_NODES; i++) {
        if (!kbd_media_only || vol_node[i].kbd_media) {
            vol_close_one(i);
        }
    }
    vol_timer_update();
}

static void vol_scan(void) {
    unsigned long   keys[KEY_MAX / (8 * sizeof(long)) + 1];
    struct input_id kid, id;
    struct stat     st;
    glob_t          found;
    bool            kbd_ext = ready && input_caps_id(kbd_rdev, &kid) &&
                              (kid.bustype == BUS_USB || kid.bustype == BUS_BLUETOOTH);

    if (glob("/dev/input/event*", 0, NULL, &found) != 0) {
        return;
    }
    for (size_t i = 0; i < found.gl_pathc; i++) {
        int  slot = -1;
        bool open_now = false;

        if (stat(found.gl_pathv[i], &st) != 0 || (ready && st.st_rdev == kbd_rdev) ||
            !input_caps_id(st.st_rdev, &id) || (id.bustype != BUS_USB && id.bustype != BUS_BLUETOOTH) ||
            !input_caps_bits(st.st_rdev, "key", keys, sizeof(keys) / sizeof(keys[0]))) {
            continue;
        }
        bool vol_keys  = BIT_SET(keys, KEY_VOLUMEUP) && BIT_SET(keys, KEY_VOLUMEDOWN);
        bool brackets  = BIT_SET(keys, KEY_LEFTBRACE) && BIT_SET(keys, KEY_RIGHTBRACE);
        bool set_vol   = dev_volume(st.st_rdev);
        bool kbd_media = kbd_ext && vol_keys && id.bustype == kid.bustype && id.vendor == kid.vendor &&
                         id.product == kid.product;

        if (!kbd_media && !(set_vol && (vol_keys || brackets))) {
            continue;
        }
        for (int j = 0; j < VOL_NODES; j++) {
            if (vol_node[j].fd >= 0 && vol_node[j].rdev == st.st_rdev) {
                open_now = true;
            } else if (vol_node[j].fd < 0 && slot < 0) {
                slot = j;
            }
        }
        if (open_now || slot < 0) {
            continue;
        }
        int fd = open(found.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);

        if (fd >= 0) {
            vol_node[slot].fd        = fd;
            vol_node[slot].rdev      = st.st_rdev;
            vol_node[slot].brackets  = set_vol;
            vol_node[slot].kbd_media = kbd_media;
            LV_LOG_USER("Volume keys%s: %s (%04x:%04x)", set_vol ? " and [ ]" : "", found.gl_pathv[i], id.vendor,
                        id.product);
        }
    }
    globfree(&found);
    vol_timer_update();
}

/* The volume nodes' events: the keys that step the volume, the others dropped. Gone (unplugged, the link lost):
 * closed, the next scan finds it again */
static void vol_read_cb(lv_timer_t *t) {
    (void)t;
    for (int i = 0; i < VOL_NODES; i++) {
        struct input_event ev;
        ssize_t            n = 0;

        while (vol_node[i].fd >= 0 && (n = read(vol_node[i].fd, &ev, sizeof(ev))) > 0) {
            int dir = ev.type == EV_KEY ? vol_dir(ev.code, vol_node[i].brackets) : 0;

            if (dir) {
                bool saver = backlight_saver_on();
                bool woke  = backlight_tick() && ev.value == 1;

                media_key(&ev, dir, woke && saver);
            }
        }
        if (vol_node[i].fd >= 0 && n < 0 && errno == ENODEV) {
            vol_close_one(i);
        }
    }
    vol_timer_update();
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

        int dir = vol_dir(ev.code, kbd_vol);

        if (dir) {
            media_key(&ev, dir, woke && saver);
            continue;   /* the volume keys, when the keyboard's own node has them; [ ] on one set to Volume */
        }
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
        if (caps && caps_digits && layout_on) {
            uint32_t digit = kbd_layout_digits_row(xkb_get_state(), ev.code);

            *key = digit ? digit : *key;                    /* a French or Belgian number row's digit (#254) */
        }
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

/* The first device matching pattern that has letters, into path; with vol_ok false not one set to Volume */
static bool find_letters(const char *pattern, char *path, size_t size, bool vol_ok) {
    glob_t      found;
    struct stat st;
    bool        ok = false;

    if (glob(pattern, 0, NULL, &found) != 0) {
        return false;
    }
    for (size_t i = 0; i < found.gl_pathc && !ok; i++) {
        if ((vol_ok || stat(found.gl_pathv[i], &st) != 0 || !dev_volume(st.st_rdev)) && has_letters(found.gl_pathv[i])) {
            snprintf(path, size, "%s", found.gl_pathv[i]);
            ok = true;
        }
    }
    globfree(&found);
    return ok;
}

/* USB keyboards by path (a wireless receiver or one with media keys has several "-kbd"
 * links: the one with the letters); Bluetooth ones have no path link: by their keys. One set to Volume
 * (a volume knob with letters) only when there is no other: beside a keyboard it is read for the volume */
static char *find_keyboard(void) {
    static char path[256];      /* a by-path link behind a hub is longer than 64 */

    for (int vol_ok = 0; vol_ok < 2; vol_ok++) {
        if (find_letters("/dev/input/by-path/*-kbd", path, sizeof(path), vol_ok) ||
            find_letters("/dev/input/event*", path, sizeof(path), vol_ok)) {
            return path;
        }
    }
    return NULL;
}

/* The keyboard open set to Volume or not: its [ ], and the paddle keyer's copy of its events (none for Volume) */
static void kbd_roles(void) {
    kbd_vol = dev_volume(kbd_rdev);
    paddle_keyer_open(kbd_vol ? NULL : kbd_path);
}

static void open_keyboard(char *path) {
    struct stat st;

    if (stat(path, &st) != 0 || !evdev_set_file(path)) {
        return;
    }
    kbd_rdev = st.st_rdev;
    kbd_ino  = st.st_ino;
    snprintf(kbd_path, sizeof(kbd_path), "%s", path);
    kbd_keys = (kbd_keys_t){0};
    hyper    = (kbd_hyper_t){0};
    dead     = false;
    caps     = false;
    caps_light();
    kbd_roles();

    if (kbd_drv.type == LV_INDEV_TYPE_NONE) {
        lv_indev_drv_init(&kbd_drv);
        kbd_drv.type    = LV_INDEV_TYPE_KEYPAD;
        kbd_drv.read_cb = kbd_read;
        lv_indev_set_group(lv_indev_drv_register(&kbd_drv), keyboard_group);
    }
    ready = true;
    layout_apply();
    vol_scan();
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
    vol_scan();            /* the media node can come after the keyboard's: looked for at every scan until found */
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
    voice_focus_init(keyboard_group);   /* spoken feedback: what the knob lands on */
    lv_msg_subscribe(MSG_USB_DEVICE_CHANGED, usb_change_cb, NULL);

    char *path = find_keyboard();

    if (path) {
        open_keyboard(path);
    }
}

bool keyboard_ready() {
    return ready;
}

/* Settings > Keys > KEYBOARD changed a device's [ ] (kbd_devices.h): the volume nodes again, the keyboard open (one set
 * to Volume gives way to one that is not) and the paddle's devices */
static void devices_apply(void) {
    vol_close(false);
    if (ready) {
        kbd_roles();
    }
    check_keyboard();
}

static void devices_cb(Subject *subj, void *user) {
    (void)subj;
    (void)user;
    devices_apply();
}

void keyboard_layout_start(void) {
    layout_on = true;
    subject_subscribe((Subject *)cfg_kbd_layout, layout_cb, NULL);
    layout_apply();
    if (cfg_kbd_dev_volume) {
        subject_subscribe((Subject *)cfg_kbd_dev_volume, devices_cb, NULL);
        devices_apply();
    }
}

size_t keyboard_devices(keyboard_dev_t *out, size_t n) {
    unsigned long   keys[KEY_MAX / (8 * sizeof(long)) + 1];
    struct input_id id;
    struct stat     st;
    glob_t          found;
    size_t          count = 0;

    if (glob("/dev/input/event*", 0, NULL, &found) != 0) {
        return 0;
    }
    for (size_t i = 0; i < found.gl_pathc; i++) {
        char id_s[KBD_DEV_ID_LEN];
        bool letters, dup = false;

        if (stat(found.gl_pathv[i], &st) != 0 || !input_caps_id(st.st_rdev, &id) ||
            (id.bustype != BUS_USB && id.bustype != BUS_BLUETOOTH) ||
            !input_caps_bits(st.st_rdev, "key", keys, sizeof(keys) / sizeof(keys[0]))) {
            continue;
        }
        letters = BIT_SET(keys, KEY_Q) && BIT_SET(keys, KEY_P) && BIT_SET(keys, KEY_SPACE);
        if (!letters && !(BIT_SET(keys, KEY_LEFTBRACE) && BIT_SET(keys, KEY_RIGHTBRACE)) &&
            !(BIT_SET(keys, KEY_VOLUMEUP) && BIT_SET(keys, KEY_VOLUMEDOWN))) {
            continue;
        }
        kbd_dev_id(id_s, id.bustype, id.vendor, id.product);
        for (size_t j = 0; j < count && !dup; j++) {
            dup = strcmp(out[j].id, id_s) == 0;
            if (dup && letters) {   /* the node with the letters names it: the others are "... Consumer Control" */
                input_caps_name(st.st_rdev, out[j].name, sizeof(out[j].name));
            }
        }
        if (dup || count >= n) {
            continue;
        }
        snprintf(out[count].id, sizeof(out[count].id), "%s", id_s);
        if (!input_caps_name(st.st_rdev, out[count].name, sizeof(out[count].name))) {
            snprintf(out[count].name, sizeof(out[count].name), "%s %04x:%04x",
                     id.bustype == BUS_BLUETOOTH ? "Bluetooth" : "USB", id.vendor, id.product);
        }
        out[count].bluetooth = id.bustype == BUS_BLUETOOTH;
        count++;
    }
    globfree(&found);
    return count;
}
