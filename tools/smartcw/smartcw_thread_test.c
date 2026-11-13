/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the USB paddle's keyer thread with SmartCW's MIDI (src/paddle_keyer.c whole, on its own
 *  thread as on the radio); see the Makefile.
 *
 *  The MIDI port is a pseudo-terminal: what the test writes on its master side is what SmartCW would send. The
 *  keyer finds it as /dev/snd/midiC1D0 and its USB device as /sys/dev/char/<its numbers>, both moved into a folder of
 *  the test's (--wrap=glob, --wrap=realpath put the folder before the paths), where a sysfs tree gives it SmartCW's
 *  USB IDs, or another device's. The key is radio_set_morse_key below; the settings and the CW message on the air are
 *  the test's variables (stub_include). The times are real: each case waits for the key with a margin.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE

#include "cfg/cfg_api.h"
#include "cw_encoder.h"
#include "input_caps.h"
#include "paddle_keyer.h"
#include "radio.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

_Atomic int cfg_cur_mode = radio_mode_cw, cfg_key_mode = radio_key_auto_left, cfg_paddle_swap, cfg_iambic_mode,
            cfg_key_speed = 20, cfg_paddle_serial;
_Atomic int cfg_paddle_map[4] = {0, 0, 1 | 2 << 16, 0};   /* MIDI: the default map, notes 1 and 2 */
float       cfg_key_ratio = 3.0f;

static _Atomic int  encoder = CW_ENCODER_IDLE;
static atomic_bool  key_on;
static _Atomic int  downs;
static char         root[512];
static int          cases, good;

void radio_set_morse_key(bool on) {
    atomic_store(&key_on, on);
    if (on) atomic_fetch_add(&downs, 1);
}

cw_encoder_state_t cw_encoder_state(void) {
    return atomic_load(&encoder);
}

bool input_caps_bits(dev_t dev, const char *what, unsigned long *bits, size_t n) {
    (void)dev, (void)what, (void)bits, (void)n;
    return false;
}

bool input_caps_id(dev_t dev, struct input_id *id) {
    (void)dev, (void)id;
    return false;
}

/* The radio's /dev and /sys, in the test's folder */
int   __real_glob(const char *pattern, int flags, int (*err)(const char *, int), glob_t *g);
char *__real_realpath(const char *path, char *resolved);

int __wrap_glob(const char *pattern, int flags, int (*err)(const char *, int), glob_t *g) {
    char p[1024];

    snprintf(p, sizeof(p), "%s%s", root, pattern);
    return __real_glob(p, flags, err, g);
}

char *__wrap_realpath(const char *path, char *resolved) {
    char p[1024];

    if (strncmp(path, "/sys/", 5) != 0) return __real_realpath(path, resolved);
    snprintf(p, sizeof(p), "%s%s", root, path);
    return __real_realpath(p, resolved);
}

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static long now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, ms % 1000 * 1000000L};

    nanosleep(&t, NULL);
}

/* The key as wanted within ms */
static bool key_within(bool want, long ms) {
    long end = now_ms() + ms;

    while (atomic_load(&key_on) != want) {
        if (now_ms() > end) return false;
        sleep_ms(1);
    }
    return true;
}

/* The key as wanted, and not down once, for ms */
static bool key_stays(bool want, long ms) {
    int  d   = atomic_load(&downs);
    long end = now_ms() + ms;

    while (now_ms() < end) {
        if (atomic_load(&key_on) != want || (!want && atomic_load(&downs) != d)) return false;
        sleep_ms(1);
    }
    return true;
}

/* How long the next key down lasts, -1 if none within ms */
static long next_element(long ms) {
    long start;

    if (!key_within(false, ms) || !key_within(true, ms)) return -1;
    start = now_ms();
    if (!key_within(false, 1000)) return -1;
    return now_ms() - start;
}

/* ---- The MIDI port: a pseudo-terminal, and the sysfs tree of its USB device ------------------------------------ */

static int master = -1;

static void mkdirs(const char *path) {
    char buf[1024];

    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void put(const char *dir, const char *name, const char *text) {
    char  path[1024];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "w")) || fputs(text, f) < 0 || fclose(f) != 0) {
        fprintf(stderr, "can't write %s: %s\n", path, strerror(errno));
        exit(2);
    }
}

static void link_to(const char *target, const char *link) {
    unlink(link);
    if (symlink(target, link) != 0) {
        fprintf(stderr, "can't link %s: %s\n", link, strerror(errno));
        exit(2);
    }
}

/* A new MIDI port on a USB device with these IDs, plugged in: the keyer finds it at its next scan */
static void plug(const char *usb, const char *vendor, const char *product) {
    char           path[1280], dev[1024], target[1024];
    struct stat    st;
    struct termios t;
    int            slave;

    if ((master = posix_openpt(O_RDWR | O_NOCTTY)) < 0 || grantpt(master) || unlockpt(master)) {
        perror("posix_openpt");
        exit(2);
    }
    /* raw: the bytes as they are written; the test's own descriptor of it is kept for the settings to stay */
    if ((slave = open(ptsname(master), O_RDWR | O_NOCTTY)) < 0 || tcgetattr(slave, &t) != 0) {
        perror("pts");
        exit(2);
    }
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    fstat(slave, &st);

    snprintf(dev, sizeof(dev), "%s/sys/devices/usb1/%s", root, usb);
    snprintf(path, sizeof(path), "%s/%s:1.1/sound/card1/midiC1D0", dev, usb);
    mkdirs(path);
    put(dev, "idVendor", vendor);
    put(dev, "idProduct", product);
    snprintf(path, sizeof(path), "%s/sys/dev/char", root);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/sys/dev/char/%u:%u", root, major(st.st_rdev), minor(st.st_rdev));
    snprintf(target, sizeof(target), "../../devices/usb1/%s/%s:1.1/sound/card1/midiC1D0", usb, usb);
    link_to(target, path);
    snprintf(path, sizeof(path), "%s/dev/snd", root);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/dev/snd/midiC1D0", root);
    link_to(ptsname(master), path);
    paddle_keyer_scan();
}

/* Unplugged: the port hangs up */
static void unplug(void) {
    char path[1024];

    snprintf(path, sizeof(path), "%s/dev/snd/midiC1D0", root);
    unlink(path);
    close(master);
    master = -1;
}

static void midi(int status, int note, int velocity) {
    unsigned char b[3] = {(unsigned char)status, (unsigned char)note, (unsigned char)velocity};

    if (write(master, b, 3) != 3) {
        perror("write");
        exit(2);
    }
}

static void on(int note) {
    midi(0x90, note, 127);
}

static void off(int note) {
    midi(0x80, note, 0);
}

static bool smartcw_within(bool want, long ms) {
    long end = now_ms() + ms;

    while (paddle_keyer_smartcw() != want) {
        if (now_ms() > end) return false;
        sleep_ms(5);
    }
    return true;
}

/* ---- The cases ------------------------------------------------------------------------------------------------- */

static void keyed(void) {
    printf("Keyed notes\n");
    on(65);
    check(key_within(true, 60), "65 on: key down");
    off(65);
    check(key_within(false, 60), "65 off: key up");
    on(64);
    check(key_within(true, 60), "64 on (its straight key): down");
    off(64);
    check(key_within(false, 60), "64 off: up");
    atomic_store(&cfg_key_mode, radio_key_manual);
    on(65);
    check(key_within(true, 60), "KEY Manual: 65 keys the same");
    off(65);
    check(key_within(false, 60), "and up");
    atomic_store(&cfg_key_mode, radio_key_auto_right);
    on(65);
    check(key_within(true, 60), "KEY Auto-R: the same");
    off(65);
    check(key_within(false, 60), "and up");
    atomic_store(&cfg_key_mode, radio_key_auto_left);
    on(66);
    check(key_stays(false, 150), "66 (PTT): nothing");
    off(66);
}

static void not_cw(void) {
    printf("Out of CW, a CW message on the air\n");
    atomic_store(&cfg_cur_mode, radio_mode_usb);
    on(65);
    check(key_stays(false, 150), "USB: 65 on keys nothing");
    off(65);
    atomic_store(&cfg_cur_mode, radio_mode_cwr);
    on(65);
    check(key_within(true, 60), "CW-R: keyed");
    atomic_store(&cfg_cur_mode, radio_mode_usb);
    check(key_within(false, 60), "the mode leaves CW with the key down: up at once");
    atomic_store(&cfg_cur_mode, radio_mode_cw);
    check(key_stays(false, 150), "back in CW, 65 still held: the rest of the element not sent");
    off(65);
    on(65);
    check(key_within(true, 60), "the next note on: keyed");
    off(65);
    check(key_within(false, 60), "up");
    atomic_store(&encoder, CW_ENCODER_BEEP);
    on(65);
    check(key_stays(false, 150), "a CW message on the air: 65 keys nothing");
    atomic_store(&encoder, CW_ENCODER_IDLE);
    check(key_stays(false, 150), "the message over, 65 still held: still nothing (no element cut short)");
    off(65);
}

static void learn(void) {
    printf("Learn\n");
    on(65);
    check(key_within(true, 60), "65 on: down");
    paddle_keyer_learn(true);
    off(65);
    check(key_within(false, 60), "Learn started, then 65 off: up (the key does not stay down)");
    on(67);
    check(key_stays(false, 150), "Learn: 67 on keys nothing");
    check(paddle_keyer_learned() == (PADDLE_SRC_MIDI << 16 | 67), "and is learned as MIDI note 67");
    off(67);
    paddle_keyer_learn(false);
}

static void paddles(void) {
    long el;

    printf("Paddle notes, the keyer here at 20 WPM (60 ms a dit)\n");
    on(67);
    el = next_element(400);
    check(el >= 40 && el <= 90, "67 held: dits (%ld ms)", el);
    off(67);
    check(key_within(false, 200) && key_stays(false, 200), "67 let go: the keyer stops");
    on(68);
    el = next_element(400);
    check(el >= 150 && el <= 220, "68 held: dahs (%ld ms)", el);
    off(68);
    check(key_within(false, 300) && key_stays(false, 200), "68 let go: stops");
    atomic_store(&cfg_paddle_swap, 1);
    on(67);
    el = next_element(400);
    check(el >= 150 && el <= 220, "Swap: 67 held gives dahs (%ld ms)", el);
    off(67);
    check(key_within(false, 300) && key_stays(false, 200), "let go: stops");
    atomic_store(&cfg_paddle_swap, 0);
}

static void two_sources(void) {
    long el;

    printf("Two sources\n");
    on(68);
    check(key_within(true, 100), "68 held: the keyer here sends a dah");
    on(65);   /* during it */
    off(68);
    check(key_within(false, 300), "65 on during the dah, 68 let go: the dah ends");
    check(key_stays(false, 200), "65 still held: not keyed after the keyer here (dropped whole)");
    off(65);
    on(65);
    check(key_within(true, 60), "65 on alone: keyed");
    on(67);
    check(key_stays(true, 200), "67 on while SmartCW keys: the keyer here waits, the key stays down");
    off(65);
    el = next_element(400);
    check(el >= 40 && el <= 90, "65 off with 67 held: the keyer here sends its dits (%ld ms)", el);
    off(67);
    check(key_within(false, 200) && key_stays(false, 200), "67 let go: up");
}

static void swr(void) {
    printf("SWR protection\n");
    on(65);
    check(key_within(true, 60), "65 on: down");
    paddle_keyer_stop();
    check(key_within(false, 60), "the protection trips: up at once");
    check(key_stays(false, 100), "65 still held: stays up");
    off(65);
    sleep_ms(50);
    on(65);
    check(key_within(true, 60), "let go and keyed again: down");
    off(65);
    check(key_within(false, 60), "up");
}

static void unplugged(void) {
    printf("Unplugged\n");
    on(65);
    check(key_within(true, 60), "65 on: down");
    unplug();
    check(key_within(false, 100), "unplugged with the key down: up at once");
    check(smartcw_within(false, 1000), "and no SmartCW on the PADDLE card");
}

static bool midi_open_within(long ms) {
    long end = now_ms() + ms;

    while (!(paddle_keyer_sources(NULL) & 1u << PADDLE_SRC_MIDI)) {
        if (now_ms() > end) return false;
        sleep_ms(5);
    }
    return true;
}

static void other_device(void) {
    printf("Another MIDI device: the MIDI map as before\n");
    plug("1-2", "abcd\n", "0001\n");
    check(midi_open_within(2000) && !paddle_keyer_smartcw(), "open, not SmartCW");
    on(65);
    check(key_stays(false, 150), "65 on: not in its map, nothing");
    off(65);
    on(1);
    check(key_within(true, 200), "note 1 (its map's dit): the keyer here sends");
    off(1);
    check(key_within(false, 200) && key_stays(false, 200), "let go: stops");
    unplug();
}

int main(int argc, char **argv) {
    snprintf(root, sizeof(root), "%s", argc > 1 ? argv[1] : "build/root");
    setvbuf(stdout, NULL, _IOLBF, 0);
    plug("1-1", "1209\n", "ff42\n");
    check(smartcw_within(true, 2000), "SmartCW's MIDI port found by its USB IDs");
    keyed();
    not_cw();
    learn();
    paddles();
    two_sources();
    swr();
    unplugged();
    other_device();
    printf("%d of %d cases right\n", good, cases);
    return good == cases ? 0 : 1;
}
