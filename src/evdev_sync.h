/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a reader's keys after its input events were lost (evdev_sync.c)
 *
 *  The kernel keeps 64 events for each reader of an input device. A reader that does not read for a while (the GUI
 *  busy, a held key's auto-repeat filling the queue) finds the oldest ones gone and EV_SYN/SYN_DROPPED in their
 *  place: a release among them never comes, and the key stays down for the reader until it is pressed again (a band
 *  key stepping bands on its own, the side PTT on the air, a paddle keying). As the kernel's documentation has it
 *  (Documentation/input/event-codes.rst): the events up to and including the next SYN_REPORT go too, the rest of a
 *  report that lost its start, then the keys are read back with EVIOCGKEY (which drops the key events still queued,
 *  so the reader goes on from what it read back). A key the reader had down that is up now is let go through the
 *  reader's own release. A key down now that the reader never saw go down is left alone: its press is gone, and
 *  acting on it now would be a press nobody just made; its release, when it comes, lets go of nothing.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <linux/input.h>    /* KEY_CNT, struct input_event */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVDEV_SYNC_LONGS ((KEY_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long)))   /* a bit for each key */

typedef struct evdev_sync {
    unsigned long down[EVDEV_SYNC_LONGS];   /* the keys down as the events read said */
    bool          lost;                     /* SYN_DROPPED read: the events up to the next SYN_REPORT go */
} evdev_sync_t;

/* What the reader does with an event it read */
typedef enum {
    EVDEV_SYNC_TAKE = 0,    /* takes it, as ever */
    EVDEV_SYNC_DROP,        /* SYN_DROPPED or the rest of the report it broke: nothing */
    EVDEV_SYNC_READ,        /* the SYN_REPORT that ends that report: the keys are read back now (evdev_sync_keys) */
} evdev_sync_use_t;

/* One event read from the device. A key's press (value 1, or 2 its auto-repeat) or release is noted in s->down */
evdev_sync_use_t evdev_sync_event(evdev_sync_t *s, const struct input_event *ev);

/* EVIOCGKEY of the device fd into now; false, all clear (every key up), when it fails */
bool evdev_sync_read(int fd, unsigned long now[EVDEV_SYNC_LONGS]);

/* The keys down now (evdev_sync_read) against those the events said. Each key down in s->down that is up in now is a
 * release lost: cleared in s->down, then given to release(code, arg), once. Returns how many; *missed (NULL: not
 * wanted) the keys down in now that s->down does not have, presses lost, not given and not noted */
int evdev_sync_keys(evdev_sync_t *s, const unsigned long now[EVDEV_SYNC_LONGS], void (*release)(uint16_t code, void *arg),
                    void *arg, int *missed);

#ifdef __cplusplus
}
#endif
