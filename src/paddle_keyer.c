/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The USB paddle (see paddle_keyer.h). A thread of its own reads the paddle from a second copy
 *  of the keyboard's events, a second keyboard (the Vail adapter's Ctrl keys), the buttons of a
 *  paddle that shows up as a mouse, a USB MIDI port and the modem lines of a USB serial port,
 *  with their real press and release, and keys the radio as the CW messages do
 *  (radio_set_morse_key). Elements are timed on CLOCK_MONOTONIC deadlines. On SmartCW's MIDI port
 *  its keyed notes key the radio as they come, its paddle notes go to the keyer here.
 */

#define _GNU_SOURCE   /* ppoll */

#include "paddle_keyer.h"

#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool paddle_smartcw(unsigned vendor, unsigned product) {
    return vendor == 0x1209 && (product == 0xff42 || product == 0xff43);
}

smartcw_note_t smartcw_note(int note, bool swap) {
    switch (note) {
        case SMARTCW_NOTE_STRAIGHT:
        case SMARTCW_NOTE_KEYER: return SMARTCW_KEYED;
        case SMARTCW_NOTE_PTT:   return SMARTCW_NONE;
    }
    switch (paddle_side(PADDLE_SRC_MIDI, note, paddle_learn_map(SMARTCW_NOTE_DIT, SMARTCW_NOTE_DAH, false), swap)) {
        case 1:  return SMARTCW_LEFT;
        case 2:  return SMARTCW_RIGHT;
        default: return SMARTCW_OTHER;
    }
}

static bool read_hex(const char *dir, const char *name, unsigned *out) {
    char  path[PATH_MAX];
    FILE *f;
    bool  ok;

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path) || !(f = fopen(path, "r"))) return false;
    ok = fscanf(f, "%x", out) == 1;
    fclose(f);
    return ok;
}

bool paddle_usb_id(const char *dir, unsigned *vendor, unsigned *product) {
    char *p = realpath(dir, NULL);
    char *end;
    bool  found = false;

    if (!p) return false;
    /* the MIDI port, its sound card and the USB interface have no IDs of their own: the USB device above them has */
    while (!(found = read_hex(p, "idVendor", vendor) && read_hex(p, "idProduct", product)) &&
           (end = strrchr(p, '/')) && end != p) {
        *end = 0;
    }
    free(p);
    return found;
}

void smartcw_keyed(smartcw_key_t *s, int note, bool on, bool busy) {
    unsigned char bit = note == SMARTCW_NOTE_STRAIGHT ? 2 : 1;

    if (on) {
        s->held |= bit;
        if (busy) s->drop = true;
    } else {
        s->held &= ~bit;
        if (!s->held) s->drop = false;   /* the next note on is an element of its own */
    }
}

void smartcw_drop(smartcw_key_t *s) {
    if (s->held) s->drop = true;
}

bool smartcw_down(const smartcw_key_t *s) {
    return s->held && !s->drop;
}

#ifndef PADDLE_IAMBIC_ONLY   /* tests/test_paddle_keyer.cpp, tools/smartcw: the logic alone */

#include "cfg/cfg_api.h"
#include "cw_encoder.h"
#include "input_caps.h"
#include "radio.h"
#include "wr12.h"

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
#include <sys/sysmacros.h>   /* major, minor */
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
static bool          sending;         /* the keyer here is on an element or its space */
static iambic_t      k;
static bool          midi_sc;         /* the MIDI port is SmartCW's */
static atomic_bool   midi_sc_shown;   /* the same, for the PADDLE card */
static smartcw_key_t sc;              /* SmartCW's keyed notes */

void paddle_keyer_stop(void) {
    atomic_store(&halt, true);
}

bool paddle_keyer_active(void) {
    int32_t m = cparam_i_get(cfg_cur_mode);

    return m == radio_mode_cw || m == radio_mode_cwr;
}

bool paddle_keyer_smartcw(void) {
    return atomic_load(&midi_sc_shown);
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

/* A paddle pressed or let go: 1 left, 2 right, 0 none */
static void press(int side, bool on) {
    if (side == 1) {
        left = on;
    } else if (side == 2) {
        right = on;
    } else {
        return;
    }
    iambic_paddles(&k, dit(), dah());
}

/* A press or release from a source: a paddle, or with Learn on what was pressed */
static void got(paddle_src_t src, int code, bool on) {
    if (atomic_load(&learning)) {
        int none = -1;

        if (on) atomic_compare_exchange_strong(&learned, &none, (int)src << 16 | code);
        return;
    }
    press(paddle_side(src, code, param_i_get(cfg_paddle_map[src]), param_i_get(cfg_paddle_swap)), on);
}

/* SmartCW's note on would be keyed late or into another source's keying: the keyer here on an element or its
 * space, another straight key down (KEY Manual), the SWR protection, not CW, or a CW message on the air (which a
 * USB paddle does not stop, so neither does SmartCW). The first source keeps the key: two at once would make
 * neither's characters, and an element of SmartCW's that came in during another's is dropped whole */
static bool smartcw_busy(void) {
    return sending || (keyed && !smartcw_down(&sc)) || atomic_load(&halt) || !paddle_keyer_active() ||
           cw_encoder_state() != CW_ENCODER_IDLE;
}

/* A MIDI note. SmartCW's keyed notes (MIDI mode "keyed") key the radio as a straight key whatever the KEY mode:
 * its keyer timed them already, and the keyer here would make its own elements of them. Its raw contacts (mode
 * "paddles") are a paddle for the keyer here. Every other MIDI device goes by the MIDI map (Learn) as before. Learn
 * takes SmartCW's presses too; their releases still let go, so a key down when Learn started does not stay down */
static void midi_note(int note, bool on) {
    smartcw_note_t what = midi_sc ? smartcw_note(note, param_i_get(cfg_paddle_swap)) : SMARTCW_OTHER;

    if (on && atomic_load(&learning)) {
        what = SMARTCW_OTHER;
    }
    switch (what) {
        case SMARTCW_KEYED: smartcw_keyed(&sc, note, on, on && smartcw_busy()); break;
        case SMARTCW_LEFT:  press(1, on); break;
        case SMARTCW_RIGHT: press(2, on); break;
        case SMARTCW_NONE:  break;
        default:            got(PADDLE_SRC_MIDI, note, on);
    }
}

/* The MIDI port just opened is on SmartCW: its character device's sysfs folder, up to the USB device */
static bool smartcw_fd(int n) {
    struct stat st;
    char        dir[64];
    unsigned    vendor, product;

    if (fstat(n, &st) != 0 || !S_ISCHR(st.st_mode)) return false;
    snprintf(dir, sizeof(dir), "/sys/dev/char/%u:%u", major(st.st_rdev), minor(st.st_rdev));
    return paddle_usb_id(dir, &vendor, &product) && paddle_smartcw(vendor, product);
}

static void fd_close(int i) {
    if (fd[i] >= 0) close(fd[i]);
    fd[i] = -1;
    left  = right = false;
    if (i == FD_MIDI) {   /* SmartCW unplugged: its key goes up as the thread looks next, at once */
        sc      = (smartcw_key_t){0};
        midi_sc = false;
        atomic_store(&midi_sc_shown, false);
    }
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
        struct timespec t = {0, left || right || keyed ? 20000000L : 500000000L};   /* keyed: a mode change soon */
        struct pollfd   p[FD_N];

        for (int i = 0; i < FD_N; i++) {
            int n = atomic_exchange(&new_fd[i], FD_NONE);

            if (n != FD_NONE) {
                fd_close(i);
                if (n >= 0) {
                    fd[i] = n;
                    atomic_fetch_or(&open_bits, 1u << i);
                }
                if (i == FD_MIDI && n >= 0 && (midi_sc = smartcw_fd(n))) {
                    atomic_store(&midi_sc_shown, true);
                    LV_LOG_USER("Paddle: the MIDI port is SmartCW's");
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
                            if (paddle_midi(&midi, buf[j], &note, &on)) midi_note(note, on);
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
            smartcw_drop(&sc);   /* the rest of an element of SmartCW's is not keyed when CW comes back */
            key(false);   /* a message on air: the press stopped it (keyboard.c), the paddle goes after */
            continue;
        }
        if (atomic_load(&halt)) {
            key(false);
            smartcw_drop(&sc);
            while (left || right || sc.held) {
                wait_until(NULL);
            }
            atomic_store(&halt, false);
            continue;
        }
        if (smartcw_down(&sc)) {
            key(true);    /* SmartCW keys: the paddles here wait until it is up */
            continue;
        }
        if (param_i_get(cfg_key_mode) == radio_key_manual) {
            key(left);    /* a straight key */
            continue;
        }
        key(false);       /* SmartCW's element ended */
        k = (iambic_t){.mode_b = param_i_get(cfg_iambic_mode) == radio_iambic_b};
        clock_gettime(CLOCK_MONOTONIC, &t);
        sending = true;
        while (!atomic_load(&halt) && (el = iambic_next(&k, dit(), dah())) != PADDLE_NONE) {
            long dit_nsec = 1200000000L / param_i_get(cfg_key_speed);

            key(true);
            wait_nsec(&t, el == PADDLE_DIT ? dit_nsec : (long)(dit_nsec * param_f_get(cfg_key_ratio)));
            key(false);
            wait_nsec(&t, dit_nsec);
        }
        sending = false;
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

/* SmartCW's own USB serial port: its text protocol, for a logger, and no paddle's contacts on its lines.
 * SmartCW keys the radio over MIDI */
static bool smartcw_tty(const char *path) {
    char     dir[96];
    unsigned vendor, product;

    snprintf(dir, sizeof(dir), "/sys/class/tty/%s/device", strrchr(path, '/') + 1);
    return paddle_usb_id(dir, &vendor, &product) && paddle_smartcw(vendor, product);
}

/* The first device of the pattern into the slot, if it is not the one open already. Not the X6200's WR12 with WR12 on
 * HOST on, a USB serial port too (#134): its port is the WR12 driver's, and its line state no paddle's contacts. Nor
 * SmartCW's (smartcw_tty) */
static void scan_one(int slot, const char *pattern, const char *pattern2, int flags) {
    glob_t      g;
    struct stat st;
    size_t      i = 0;
    int         n;

    if (glob(pattern, 0, NULL, &g) != 0 && (!pattern2 || glob(pattern2, 0, NULL, &g) != 0)) return;
    while (i < g.gl_pathc && slot == FD_SER && (wr12_port(g.gl_pathv[i]) || smartcw_tty(g.gl_pathv[i]))) i++;
    if (i < g.gl_pathc && stat(g.gl_pathv[i], &st) == 0 && !is_open(slot, &st) && (n = open(g.gl_pathv[i], flags)) >= 0) {
        LV_LOG_USER("Paddle on %s", g.gl_pathv[i]);
        if (slot == FD_SER) snprintf(ser_name, sizeof(ser_name), "%s", g.gl_pathv[i] + 5);   /* no /dev/ */
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
