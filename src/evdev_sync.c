/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a reader's keys after its input events were lost (see evdev_sync.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "evdev_sync.h"

#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>

#define LONG_BITS (8 * sizeof(long))

evdev_sync_use_t evdev_sync_event(evdev_sync_t *s, const struct input_event *ev) {
    if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
        s->lost = true;
        return EVDEV_SYNC_DROP;
    }
    if (s->lost) {
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
            s->lost = false;
            return EVDEV_SYNC_READ;
        }
        return EVDEV_SYNC_DROP;
    }
    if (ev->type == EV_KEY && ev->code < KEY_CNT) {
        unsigned long bit = 1UL << (ev->code % LONG_BITS);

        if (ev->value) {
            s->down[ev->code / LONG_BITS] |= bit;
        } else {
            s->down[ev->code / LONG_BITS] &= ~bit;
        }
    }
    return EVDEV_SYNC_TAKE;
}

bool evdev_sync_read(int fd, unsigned long now[EVDEV_SYNC_LONGS]) {
    memset(now, 0, EVDEV_SYNC_LONGS * sizeof(long));
    if (ioctl(fd, EVIOCGKEY(EVDEV_SYNC_LONGS * sizeof(long)), now) < 0) {
        memset(now, 0, EVDEV_SYNC_LONGS * sizeof(long));    /* a part read is no state either */
        return false;
    }
    return true;
}

int evdev_sync_keys(evdev_sync_t *s, const unsigned long now[EVDEV_SYNC_LONGS], void (*release)(uint16_t code, void *arg),
                    void *arg, int *missed) {
    int released = 0, pressed = 0;

    for (size_t i = 0; i < EVDEV_SYNC_LONGS; i++) {
        unsigned long up = s->down[i] & ~now[i], new_down = now[i] & ~s->down[i];

        for (; new_down; new_down &= new_down - 1) {
            pressed++;
        }
        for (size_t b = 0; up; b++, up >>= 1) {
            if (up & 1) {
                uint16_t code = (uint16_t)(i * LONG_BITS + b);

                s->down[i] &= ~(1UL << b);  /* first: a release() that reads the keys again finds it up */
                released++;
                release(code, arg);
            }
        }
    }
    if (missed) {
        *missed = pressed;
    }
    return released;
}
