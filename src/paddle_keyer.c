/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The USB paddle (see paddle_keyer.h). A thread of its own reads the paddle from a second copy
 *  of the keyboard's events, a second keyboard (the Vail adapter's Ctrl keys), the buttons of a
 *  paddle that shows up as a mouse, a USB MIDI port and the modem lines of a USB serial port,
 *  with their real press and release, and keys the radio as the CW messages do
 *  (radio_set_morse_key). Elements are timed on CLOCK_MONOTONIC deadlines.
 */

#define _GNU_SOURCE   /* ppoll */

#include "paddle_keyer.h"

#include <linux/input.h>

/* The paddle that is not the element's own: its press is the one remembered */
static bool other_held(const iambic_t *k, bool dit, bool dah) {
    return k->last == PADDLE_DIT ? dah : k->last == PADDLE_DAH ? dit : false;
}

void iambic_paddles(iambic_t *k, bool dit, bool dah) {
    bool o = other_held(k, dit, dah);

    if (!o) {
        k->other_on = false;    /* let go: pressed again, it is a press of its own */
    }
    if (k->last == PADDLE_DIT && dah) {
        k->dah_mem = true;
    }
    if (k->last == PADDLE_DAH && dit) {
        k->dit_mem = true;
    }
    if (dit && dah) {
        k->squeeze = true;
        k->kept    = false;
    } else if (k->squeeze && o && !k->other_on) {
        /* The squeeze ended with the other paddle still held, and that one was pressed during
         * the element: a press of its own that only overlapped. Right-left-right for a K with
         * the dit down just before the dah is let go: mode A dropped the dit, and sent M */
        k->kept = true;
    }
}

paddle_el_t iambic_next(iambic_t *k, bool dit, bool dah) {
    paddle_el_t next;

    iambic_paddles(k, dit, dah);
    /* A squeeze let go: mode A ends with the element just sent, B sends the other one too */
    if (!k->mode_b && k->squeeze && !dit && !dah && !k->kept) {
        k->dit_mem = k->dah_mem = false;
    }
    if (k->dit_mem || k->dah_mem) {
        next = k->dit_mem ? PADDLE_DIT : PADDLE_DAH;
    } else if (dit) {   /* a squeeze from idle starts with a dit */
        next = PADDLE_DIT;
    } else if (dah) {
        next = PADDLE_DAH;
    } else {
        next = PADDLE_NONE;
    }
    k->dit_mem = k->dah_mem = k->squeeze = k->kept = false;
    k->last     = next;
    k->other_on = other_held(k, dit, dah);
    iambic_paddles(k, dit, dah);   /* still squeezed as the new element starts */
    return next;
}

int paddle_side(paddle_src_t src, int code, int map, bool swap) {
    int side = 0;

    if (src == PADDLE_SRC_KBD && code == KEY_LEFTBRACE) {
        side = 1;
    } else if (src == PADDLE_SRC_KBD && code == KEY_RIGHTBRACE) {
        side = 2;
    } else if (code == (map & 0xffff)) {
        side = 1;
    } else if (code == map >> 16) {
        side = 2;
    }
    return side && swap ? 3 - side : side;
}

int paddle_learn_map(int dit, int dah, bool flip) {
    return flip ? dah | dit << 16 : dit | dah << 16;
}

bool paddle_midi(paddle_midi_t *m, unsigned char b, int *note, bool *on) {
    if (b >= 0xf8) return false;                  /* clock and the like, anywhere */
    if (b & 0x80) {                               /* a status; system ones (sysex...) are skipped */
        m->status = b < 0xf0 ? b : 0;
        m->n      = 0;
        return false;
    }
    if (!m->status) return false;
    m->data[m->n++] = b;
    if ((m->status & 0xe0) == 0xc0) {             /* program change, channel pressure: one byte */
        m->n = 0;
        return false;
    }
    if (m->n < 2) return false;
    m->n = 0;                                     /* running status: the next pair is the same kind */
    if ((m->status & 0xe0) != 0x80) return false; /* not note on (0x9n) or off (0x8n) */
    *note = m->data[0];
    *on   = (m->status & 0xf0) == 0x90 && m->data[1] > 0;
    return true;
}

bool paddle_mouse(bool btn_left, bool btn_right, bool rel_x, bool adapter, unsigned vendor, unsigned product) {
    /* with pointer axes: a keyer adapter, or the HOTSPOTEK paddle (HID only) */
    return btn_left && btn_right && (!rel_x || adapter || (vendor == 0x1500 && product == 0x3001));
}

#ifndef PADDLE_IAMBIC_ONLY   /* tests/test_paddle_keyer.cpp: the logic alone */

#include "cfg/cfg_api.h"
#include "cw_encoder.h"
#include "input_caps.h"
#include "radio.h"

#include "lvgl/lvgl.h"

#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The devices read, and the source of each */
enum { FD_KBD, FD_BTN, FD_KBD2, FD_MIDI, FD_SER, FD_N };

static const paddle_src_t fd_src[FD_N] = {PADDLE_SRC_KBD, PADDLE_SRC_BTN, PADDLE_SRC_KBD, PADDLE_SRC_MIDI,
                                          PADDLE_SRC_SER};
static const int ser_lines[] = {TIOCM_CTS, TIOCM_DSR, TIOCM_CD, TIOCM_RI};

#define FD_NONE  -1
#define FD_CLOSE -2     /* new_fd: close the one open */

static atomic_int  new_fd[FD_N] = {FD_NONE, FD_NONE, FD_NONE, FD_NONE, FD_NONE};  /* for the thread to take */
static atomic_uint open_bits;   /* 1 << FD_* open */
static atomic_bool halt;        /* paddle_keyer_stop() */
static atomic_bool learning;
static atomic_int  learned = -1;
static int           fd[FD_N] = {-1, -1, -1, -1, -1};
static dev_t         fd_rdev[FD_N];   /* what the scan handed over: a new one (unplugged and back) opens again */
static ino_t         fd_ino[FD_N];
static char          ser_name[32];
static int           ser_last;        /* modem lines as last read */
static paddle_midi_t midi;
static bool          left, right;     /* the paddles held */
static bool          keyed;
static iambic_t      k;

void paddle_keyer_stop(void) {
    atomic_store(&halt, true);
}

bool paddle_keyer_active(void) {
    int32_t m = cparam_i_get(cfg_cur_mode);

    return m == radio_mode_cw || m == radio_mode_cwr;
}

bool paddle_keyer_key(int code) {
    return paddle_keyer_active() &&
           paddle_side(PADDLE_SRC_KBD, code, param_i_get(cfg_paddle_map[PADDLE_SRC_KBD]), false);
}

void paddle_keyer_learn(bool on) {
    atomic_store(&learned, -1);
    atomic_store(&learning, on);
}

int paddle_keyer_learned(void) {
    return atomic_exchange(&learned, -1);
}

unsigned paddle_keyer_sources(const char **tty) {
    unsigned o = atomic_load(&open_bits), s = 0;

    for (int i = 0; i < FD_N; i++) {
        if (o & 1u << i) s |= 1u << fd_src[i];
    }
    if (tty) *tty = ser_name;
    return s;
}

static bool dit(void) {
    return param_i_get(cfg_key_mode) == radio_key_auto_right ? right : left;
}

static bool dah(void) {
    return param_i_get(cfg_key_mode) == radio_key_auto_right ? left : right;
}

static void key(bool on) {
    if (on != keyed) {
        keyed = on;
        radio_set_morse_key(on);
    }
}

/* A press or release from a source: a paddle, or with Learn on what was pressed */
static void got(paddle_src_t src, int code, bool on) {
    int side;

    if (atomic_load(&learning)) {
        int none = -1;

        if (on) atomic_compare_exchange_strong(&learned, &none, (int)src << 16 | code);
        return;
    }
    side = paddle_side(src, code, param_i_get(cfg_paddle_map[src]), param_i_get(cfg_paddle_swap));
    if (side == 1) {
        left = on;
    } else if (side == 2) {
        right = on;
    } else {
        return;
    }
    iambic_paddles(&k, dit(), dah());
}

static void fd_close(int i) {
    if (fd[i] >= 0) close(fd[i]);
    fd[i] = -1;
    left  = right = false;
    atomic_fetch_and(&open_bits, ~(1u << i));
}

static void ser_read(void) {
    int m;

    if (ioctl(fd[FD_SER], TIOCMGET, &m) < 0) {   /* unplugged */
        fd_close(FD_SER);
        return;
    }
    for (size_t j = 0; j < sizeof(ser_lines) / sizeof(ser_lines[0]); j++) {
        if ((m ^ ser_last) & ser_lines[j]) got(PADDLE_SRC_SER, ser_lines[j], m & ser_lines[j]);
    }
    ser_last = m;
}

/* Reads the paddle until the deadline; with none, until something comes (a held paddle looks
 * again soon: it may be waiting for a CW message it stopped) */
static void wait_until(const struct timespec *deadline) {
    for (;;) {
        struct timespec t = {0, left || right ? 20000000L : 500000000L};
        struct pollfd   p[FD_N];

        for (int i = 0; i < FD_N; i++) {
            int n = atomic_exchange(&new_fd[i], FD_NONE);

            if (n != FD_NONE) {
                fd_close(i);
                if (n >= 0) {
                    fd[i] = n;
                    atomic_fetch_or(&open_bits, 1u << i);
                }
                midi = (paddle_midi_t){0};
                if (i == FD_SER && n >= 0 && ioctl(n, TIOCMGET, &ser_last) < 0) ser_last = 0;
            }
        }
        if (deadline) {
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            t.tv_sec  = deadline->tv_sec - now.tv_sec;
            t.tv_nsec = deadline->tv_nsec - now.tv_nsec;
            if (t.tv_nsec < 0) {
                t.tv_sec--;
                t.tv_nsec += 1000000000L;
            }
            if (t.tv_sec < 0) return;
        }
        /* ponytail: the modem lines are polled every 2 ms; a TIOCMIWAIT thread if it ever matters */
        if (fd[FD_SER] >= 0 && (t.tv_sec > 0 || t.tv_nsec > 2000000L)) {
            t.tv_sec  = 0;
            t.tv_nsec = 2000000L;
        }
        for (int i = 0; i < FD_N; i++) {   /* -1: left out; the serial data is not read, only a hangup */
            p[i] = (struct pollfd){fd[i], i == FD_SER ? 0 : POLLIN, 0};
        }
        if (ppoll(p, FD_N, &t, NULL) > 0) {
            for (int i = 0; i < FD_N; i++) {
                struct input_event in;
                unsigned char      buf[64];
                ssize_t            n;
                int                note;
                bool               on;

                if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {   /* the device is gone */
                    fd_close(i);
                }
                if (i == FD_MIDI) {
                    while (fd[i] >= 0 && (n = read(fd[i], buf, sizeof(buf))) > 0) {
                        for (ssize_t j = 0; j < n; j++) {
                            if (paddle_midi(&midi, buf[j], &note, &on)) got(PADDLE_SRC_MIDI, note, on);
                        }
                    }
                    continue;
                }
                while (i != FD_SER && fd[i] >= 0 && read(fd[i], &in, sizeof(in)) == sizeof(in)) {
                    if (in.type == EV_KEY && in.value != 2) got(fd_src[i], in.code, in.value);   /* 2: auto-repeat */
                }
            }
        }
        if (fd[FD_SER] >= 0) ser_read();
        if (!deadline) return;
    }
}

static void wait_nsec(struct timespec *t, long nsec) {
    t->tv_nsec += nsec;
    t->tv_sec += t->tv_nsec / 1000000000L;
    t->tv_nsec %= 1000000000L;
    wait_until(t);
}

static void * keyer_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec t;
        paddle_el_t     el;

        wait_until(NULL);
        if (!paddle_keyer_active() || cw_encoder_state() != CW_ENCODER_IDLE) {
            key(false);   /* a message on air: the press stopped it (keyboard.c), the paddle goes after */
            continue;
        }
        if (atomic_load(&halt)) {
            key(false);
            while (left || right) {
                wait_until(NULL);
            }
            atomic_store(&halt, false);
            continue;
        }
        if (param_i_get(cfg_key_mode) == radio_key_manual) {
            key(left);    /* a straight key */
            continue;
        }
        k = (iambic_t){.mode_b = param_i_get(cfg_iambic_mode) == radio_iambic_b};
        clock_gettime(CLOCK_MONOTONIC, &t);
        while (!atomic_load(&halt) && (el = iambic_next(&k, dit(), dah())) != PADDLE_NONE) {
            long dit_nsec = 1200000000L / param_i_get(cfg_key_speed);

            key(true);
            wait_nsec(&t, el == PADDLE_DIT ? dit_nsec : (long)(dit_nsec * param_f_get(cfg_key_ratio)));
            key(false);
            wait_nsec(&t, dit_nsec);
        }
    }
    return NULL;
}

static void hand_over(int i, int n) {
    static pthread_t thread;
    static bool      started;
    struct stat      st;

    if (n >= 0 && fstat(n, &st) == 0) {
        fd_rdev[i] = st.st_rdev;
        fd_ino[i]  = st.st_ino;
    } else if (n == FD_CLOSE) {
        fd_rdev[i] = 0;
        fd_ino[i]  = 0;
    }
    if (n != FD_NONE) {
        int old = atomic_exchange(&new_fd[i], n);

        if (old >= 0) close(old);
    }
    if (!started) {
        started = pthread_create(&thread, NULL, keyer_thread, NULL) == 0;
    }
}

void paddle_keyer_open(const char *path) {
    hand_over(FD_KBD, open(path, O_RDONLY | O_NONBLOCK));
}

/* The device is the one slot i has open */
static bool is_open(int i, const struct stat *st) {
    return (atomic_load(&open_bits) & 1u << i) && st->st_rdev == fd_rdev[i] && st->st_ino == fd_ino[i];
}

#define BITS(max) ((max) / (8 * sizeof(long)) + 1)

static bool has_bit(const unsigned long *b, int n) {
    return n >= 0 && n <= KEY_MAX && b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long))) & 1;
}

/* The USB device of the input has a serial port or MIDI too: a keyer adapter on a microcontroller
 * (Arduino, TinyUSB). Mice and keyboard/mouse receivers are HID only */
static bool adapter(const char *dev) {
    char   pat[96];
    glob_t g;
    bool   found = false;

    snprintf(pat, sizeof(pat), "/sys/class/input/%s/device/device/../../*:*/bInterfaceClass", strrchr(dev, '/') + 1);
    if (glob(pat, 0, NULL, &g) != 0) return false;
    for (size_t i = 0; i < g.gl_pathc && !found; i++) {
        FILE    *f = fopen(g.gl_pathv[i], "r");
        unsigned c = 0;

        if (f) {
            found = fscanf(f, "%x", &c) == 1 && (c == 0x01 || c == 0x02);   /* audio (MIDI), CDC */
            fclose(f);
        }
    }
    globfree(&g);
    return found;
}

/* A mouse-button paddle, and a keyboard with the paddle keys other than the one keyboard.c reads.
 * Only USB and Bluetooth ones: the radio's own keys (matrix_keypad) have [ ] and Left Ctrl codes.
 * What each device has comes from sysfs, and only the one taken for a slot is opened: opening a USB
 * mouse to look stalled the GUI 50 ms a node, on every scan after a plug (#79, input_caps.h) */
static void scan_input(void) {
    int    map = param_i_get(cfg_paddle_map[PADDLE_SRC_KBD]);
    bool   found[FD_N] = {false};
    glob_t g;

    if (glob("/dev/input/event*", 0, NULL, &g) != 0) return;
    for (size_t i = 0; i < g.gl_pathc && !(found[FD_BTN] && found[FD_KBD2]); i++) {
        unsigned long   keys[BITS(KEY_MAX)], rel[BITS(REL_MAX)], abs[BITS(ABS_MAX)];
        struct input_id id;
        struct stat     st;
        int             slot = -1;
        int             n;
        bool            adp;

        if (stat(g.gl_pathv[i], &st) != 0 || !input_caps_bits(st.st_rdev, "key", keys, BITS(KEY_MAX))) continue;
        input_caps_bits(st.st_rdev, "rel", rel, BITS(REL_MAX));
        input_caps_bits(st.st_rdev, "abs", abs, BITS(ABS_MAX));
        input_caps_id(st.st_rdev, &id);
        adp = id.bustype == BUS_USB && adapter(g.gl_pathv[i]);
        if (!found[FD_BTN] && paddle_mouse(has_bit(keys, BTN_LEFT), has_bit(keys, BTN_RIGHT),
                                           has_bit(rel, REL_X) || has_bit(abs, ABS_X), adp, id.vendor, id.product)) {
            slot = FD_BTN;
        } else if (!found[FD_KBD2] && (id.bustype == BUS_USB || id.bustype == BUS_BLUETOOTH) &&
                   !is_open(FD_KBD, &st) &&
                   ((adp && !has_bit(keys, BTN_LEFT)) ||   /* an adapter's keys, any: Learn sees them */
                    (has_bit(keys, map & 0xffff) && has_bit(keys, map >> 16)) ||
                    (has_bit(keys, KEY_LEFTBRACE) && has_bit(keys, KEY_RIGHTBRACE)))) {
            slot = FD_KBD2;
        }
        if (slot < 0) continue;
        if (is_open(slot, &st)) {
            found[slot] = true;
        } else if ((n = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK)) >= 0) {   /* failing: the next one */
            found[slot] = true;
            LV_LOG_USER("Paddle on %s: %s (%04x:%04x)", slot == FD_BTN ? "mouse buttons" : "keys",
                        g.gl_pathv[i], id.vendor, id.product);
            hand_over(slot, n);
        }
    }
    globfree(&g);
}

/* The first device of the pattern into the slot, if it is not the one open already */
static void scan_one(int slot, const char *pattern, const char *pattern2, int flags) {
    glob_t      g;
    struct stat st;
    int         n;

    if (glob(pattern, 0, NULL, &g) != 0 && (!pattern2 || glob(pattern2, 0, NULL, &g) != 0)) return;
    if (stat(g.gl_pathv[0], &st) == 0 && !is_open(slot, &st) && (n = open(g.gl_pathv[0], flags)) >= 0) {
        LV_LOG_USER("Paddle on %s", g.gl_pathv[0]);
        if (slot == FD_SER) snprintf(ser_name, sizeof(ser_name), "%s", g.gl_pathv[0] + 5);   /* no /dev/ */
        hand_over(slot, n);
    }
    globfree(&g);
}

void paddle_keyer_scan(void) {
    scan_input();
    scan_one(FD_MIDI, "/dev/snd/midiC*D*", NULL, O_RDONLY | O_NONBLOCK);
    if (param_i_get(cfg_paddle_serial)) {   /* opening it raises DTR and RTS: the supply of many interfaces */
        scan_one(FD_SER, "/dev/ttyUSB*", "/dev/ttyACM*", O_RDWR | O_NOCTTY | O_NONBLOCK);
    } else if (atomic_load(&open_bits) & 1u << FD_SER) {
        hand_over(FD_SER, FD_CLOSE);
    }
}

#endif
