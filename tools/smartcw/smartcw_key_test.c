/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of SmartCW's MIDI on the USB paddle (src/paddle_keyer.c, its logic alone with
 *  PADDLE_IAMBIC_ONLY); see the Makefile.
 *
 *  SmartCW's bytes as its INTEROP.md shows them, through the MIDI reader; what each of its notes does (keyed, a
 *  paddle with and without Swap, its PTT, other numbers); its keyed notes as a straight key: down and up with them, an
 *  element that starts while another source has the key dropped whole, the key up at once and the rest of the element
 *  left out when it has to go up (not CW, the SWR protection, unplugged); and SmartCW found by its USB IDs from a MIDI
 *  port's sysfs folder, in a sysfs tree made under build/.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "paddle_keyer.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- The MIDI reader on SmartCW's bytes ------------------------------------------------------------------------- */

typedef struct {
    int  note;
    bool on;
} ev_t;

static int feed(const unsigned char *bytes, size_t n, ev_t *out) {
    paddle_midi_t m = {0};
    int           got = 0;

    for (size_t i = 0; i < n; i++) {
        int  note;
        bool on;

        if (paddle_midi(&m, bytes[i], &note, &on)) out[got++] = (ev_t){note, on};
    }
    return got;
}

static void midi_bytes(void) {
    /* INTEROP.md: a dit, then the PTT pulse; note on 127, note off 0, channel 1 */
    static const unsigned char dit_ptt[] = {0x90, 0x41, 0x7f, 0x80, 0x41, 0x00, 0x90, 0x42, 0x7f, 0x80, 0x42, 0x00};
    /* the same element as a note on with velocity 0, and running status; a clock byte in the middle */
    static const unsigned char running[] = {0x90, 0x40, 0x7f, 0xf8, 0x40, 0x00, 0x43, 0x7f, 0x43, 0x00};
    ev_t                       ev[8];
    int                        n;

    printf("SmartCW's MIDI bytes\n");
    n = feed(dit_ptt, sizeof(dit_ptt), ev);
    check(n == 4 && ev[0].note == SMARTCW_NOTE_KEYER && ev[0].on && ev[1].note == SMARTCW_NOTE_KEYER && !ev[1].on,
          "90 41 7F, 80 41 00: note 65 on, then off");
    check(n == 4 && ev[2].note == SMARTCW_NOTE_PTT && ev[2].on && !ev[3].on, "90 42 7F, 80 42 00: note 66 on, off");
    n = feed(running, sizeof(running), ev);
    check(n == 4 && ev[0].note == 64 && ev[0].on && ev[1].note == 64 && !ev[1].on,
          "running status, velocity 0 as the note off, a clock byte between: 64 on, off");
    check(n == 4 && ev[2].note == 67 && ev[2].on && ev[3].note == 67 && !ev[3].on, "then 67 on, off");
}

/* ---- What each note does ---------------------------------------------------------------------------------------- */

static void note_map(void) {
    printf("SmartCW's notes\n");
    check(smartcw_note(65, false) == SMARTCW_KEYED && smartcw_note(65, true) == SMARTCW_KEYED,
          "65 (its keyer, text, memories, beacon): keys the radio, Swap or not");
    check(smartcw_note(64, false) == SMARTCW_KEYED && smartcw_note(64, true) == SMARTCW_KEYED,
          "64 (its straight key): keys the radio, Swap or not");
    check(smartcw_note(67, false) == SMARTCW_LEFT && smartcw_note(68, false) == SMARTCW_RIGHT,
          "67 dit, 68 dah: the left and the right paddle of the keyer here");
    check(smartcw_note(67, true) == SMARTCW_RIGHT && smartcw_note(68, true) == SMARTCW_LEFT,
          "with Swap: 67 right, 68 left");
    check(smartcw_note(66, false) == SMARTCW_NONE && smartcw_note(66, true) == SMARTCW_NONE, "66 (PTT): nothing");
    check(smartcw_note(0, false) == SMARTCW_OTHER && smartcw_note(1, false) == SMARTCW_OTHER &&
              smartcw_note(2, false) == SMARTCW_OTHER && smartcw_note(69, false) == SMARTCW_OTHER &&
              smartcw_note(63, false) == SMARTCW_OTHER,
          "0, 1, 2 (its other note preset), 63, 69: as any MIDI note, by the MIDI map");
}

/* ---- The keyed notes as a straight key -------------------------------------------------------------------------- */

static void straight(void) {
    smartcw_key_t s = {0};

    printf("Keyed notes as a straight key\n");
    smartcw_keyed(&s, 65, true, false);
    check(smartcw_down(&s), "65 on: key down");
    smartcw_keyed(&s, 65, false, false);
    check(!smartcw_down(&s), "65 off: key up");

    smartcw_keyed(&s, 64, true, false);
    check(smartcw_down(&s), "64 on: down");
    smartcw_keyed(&s, 65, true, false);
    smartcw_keyed(&s, 64, false, false);
    check(smartcw_down(&s), "65 on, 64 off: still down (65 held)");
    smartcw_keyed(&s, 65, false, false);
    check(!smartcw_down(&s) && !s.held && !s.drop, "65 off: up, nothing held");

    /* the radio's own keyer on an element (or a CW message, not CW): SmartCW's element is dropped whole */
    smartcw_keyed(&s, 65, true, true);
    check(!smartcw_down(&s), "65 on while busy: not keyed");
    check(!smartcw_down(&s) && s.held, "busy over, 65 still held: still not keyed (no element cut short)");
    smartcw_keyed(&s, 64, true, false);
    check(!smartcw_down(&s), "64 on as well, not busy: still not, up to the note off of both");
    smartcw_keyed(&s, 65, false, false);
    check(!smartcw_down(&s), "65 off, 64 held: not");
    smartcw_keyed(&s, 64, false, false);
    smartcw_keyed(&s, 65, true, false);
    check(smartcw_down(&s), "both off, then 65 on: keyed again");
    smartcw_keyed(&s, 65, false, false);
    check(!smartcw_down(&s), "65 off: up");

    /* the key up for another reason (not CW, the SWR protection): at once, and the rest not keyed */
    smartcw_keyed(&s, 65, true, false);
    smartcw_drop(&s);
    check(!smartcw_down(&s), "down, then dropped: up at once");
    smartcw_keyed(&s, 65, false, false);
    smartcw_keyed(&s, 65, true, false);
    check(smartcw_down(&s), "its note off, then the next on: keyed");
    smartcw_keyed(&s, 65, false, false);
    smartcw_drop(&s);
    smartcw_keyed(&s, 65, true, false);
    check(smartcw_down(&s), "dropped with nothing held: the next note on is keyed (nothing left over)");
    smartcw_keyed(&s, 65, false, false);

    /* SmartCW unplugged: the reader starts over (fd_close) */
    smartcw_keyed(&s, 65, true, false);
    s = (smartcw_key_t){0};
    check(!smartcw_down(&s), "unplugged while down: up");

    /* text from SmartCW: "E E" as elements and gaps, keyed one for one */
    {
        int downs = 0, ups = 0;

        for (int i = 0; i < 2; i++) {
            smartcw_keyed(&s, 65, true, false);
            downs += smartcw_down(&s);
            smartcw_keyed(&s, 65, false, false);
            ups += !smartcw_down(&s);
        }
        check(downs == 2 && ups == 2, "two elements of text: two downs, two ups");
    }
}

/* ---- SmartCW found by its USB IDs ------------------------------------------------------------------------------- */

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (!f || fputs(text, f) < 0 || fclose(f) != 0) {
        fprintf(stderr, "can't write %s: %s\n", path, strerror(errno));
        exit(2);
    }
}

static void mkdirs(const char *path) {
    char buf[512];

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

/* A USB device with a MIDI port, as sysfs has one: the port's class link, its folder under the sound card under the
 * USB interface under the USB device, which alone has idVendor and idProduct */
static void usb_device(const char *root, const char *name, const char *vendor, const char *product) {
    char dev[512], path[600], link[600], target[600];

    snprintf(dev, sizeof(dev), "%s/devices/usb1/%s", root, name);
    snprintf(path, sizeof(path), "%s/%s:1.1/sound/card1/midiC1D0", dev, name);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/%s:1.1/bInterfaceClass", dev, name);
    put(path, "01\n");
    if (vendor) {
        snprintf(path, sizeof(path), "%s/idVendor", dev);
        put(path, vendor);
        snprintf(path, sizeof(path), "%s/idProduct", dev);
        put(path, product);
    }
    snprintf(path, sizeof(path), "%s/class/sound", root);
    mkdirs(path);
    snprintf(link, sizeof(link), "%s/class/sound/%s", root, name);
    snprintf(target, sizeof(target), "../../devices/usb1/%s/%s:1.1/sound/card1/midiC1D0", name, name);
    unlink(link);
    if (symlink(target, link) != 0) {
        fprintf(stderr, "can't link %s: %s\n", link, strerror(errno));
        exit(2);
    }
}

static bool is_smartcw(const char *root, const char *name, unsigned *vendor, unsigned *product) {
    char link[600];

    snprintf(link, sizeof(link), "%s/class/sound/%s", root, name);
    *vendor = *product = 0;
    return paddle_usb_id(link, vendor, product) && paddle_smartcw(*vendor, *product);
}

static void usb_ids(const char *root) {
    unsigned v, p;
    char     dev[600];

    printf("SmartCW by its USB IDs\n");
    usb_device(root, "1-1", "1209\n", "ff42\n");
    usb_device(root, "1-2", "1209\n", "ff43\n");
    usb_device(root, "1-3", "1209\n", "ff41\n");
    usb_device(root, "1-4", "abcd\n", "ff42\n");
    usb_device(root, "1-5", NULL, NULL);
    check(is_smartcw(root, "1-1", &v, &p) && v == 0x1209 && p == 0xff42,
          "1209:ff42, from the MIDI port up through card and interface to the device: SmartCW");
    check(is_smartcw(root, "1-2", &v, &p) && p == 0xff43, "1209:ff43 (its keyboard on): SmartCW");
    check(!is_smartcw(root, "1-3", &v, &p) && v == 0x1209 && p == 0xff41, "1209:ff41: found, not SmartCW");
    check(!is_smartcw(root, "1-4", &v, &p) && v == 0xabcd, "ff42 of another vendor ID: not SmartCW");
    snprintf(dev, sizeof(dev), "%s/class/sound/1-5", root);
    check(!paddle_usb_id(dev, &v, &p), "no IDs up the tree: none");
    check(!paddle_usb_id("/nonexistent/sys/class/sound/midiC9D0", &v, &p), "a folder that is not there: none");
    snprintf(dev, sizeof(dev), "%s/devices/usb1/1-1", root);
    check(paddle_usb_id(dev, &v, &p) && v == 0x1209, "the USB device's own folder: its IDs");
    check(paddle_smartcw(0x1209, 0xff42) && paddle_smartcw(0x1209, 0xff43) && !paddle_smartcw(0x1209, 0) &&
              !paddle_smartcw(0, 0xff42),
          "paddle_smartcw: 1209 with ff42 or ff43 only");
}

int main(int argc, char **argv) {
    char root[256];

    snprintf(root, sizeof(root), "%s/sys", argc > 1 ? argv[1] : "build");
    midi_bytes();
    note_map();
    straight();
    usb_ids(root);
    printf("%d of %d cases right\n", good, cases);
    return good == cases ? 0 : 1;
}
