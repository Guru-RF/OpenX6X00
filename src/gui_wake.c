/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The GUI loop's sleep (gui_wake.h)
 */
#include "gui_wake.h"

#include "lvgl/lvgl.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define MAX_FDS 12

static struct pollfd fds[MAX_FDS];
static int           nfds;
static int           efd = -1, ifd = -1;   /* events, input from threads */
static bool          draw_pending;
static uint32_t      draw_at;               /* lv_tick of the put-off draw */
static uint32_t      draw_after;            /* the refresh timer's last_run when it was put off */

void gui_wake_init(void) {
    efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ifd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0 || ifd < 0) {
        perror("gui_wake: eventfd");
        return;
    }
    fds[nfds++] = (struct pollfd){.fd = efd, .events = POLLIN};
    fds[nfds++] = (struct pollfd){.fd = ifd, .events = POLLIN};
}

void gui_wake_add_fd(int fd) {
    if (fd >= 0 && nfds < MAX_FDS) {
        fds[nfds++] = (struct pollfd){.fd = fd, .events = POLLIN};
    }
}

static void post(int fd) {
    uint64_t one = 1;

    if (fd >= 0 && write(fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
        /* the counter is full: the GUI is woken anyway */
    }
}

void gui_wake(void) {
    post(efd);
}

void gui_wake_input(void) {
    post(ifd);
}

gui_wake_t gui_wake_wait(int32_t ms) {
    if (!nfds) {
        usleep(ms * 1000);
        return WAKE_NONE;
    }
    int n = poll(fds, nfds, ms);

    if (n <= 0) {
        return WAKE_NONE;
    }
    gui_wake_t woken = WAKE_NONE;

    for (int i = 0; i < nfds; i++) {
        short r = fds[i].revents;

        if (!r) continue;
        if (fds[i].fd == efd || fds[i].fd == ifd) {
            uint64_t v;

            if (read(fds[i].fd, &v, sizeof(v)) < 0) {}
            if (fds[i].fd == ifd) woken = WAKE_INPUT;
            else if (!woken) woken = WAKE_EVENT;
        } else if (r & (POLLERR | POLLHUP | POLLNVAL)) {
            /* Gone (unplugged, a driver rebind) or failed: dropped, or poll would return at once forever. Before
             * POLLIN: evdev gives a removed device that still had events POLLIN too, and its read() fails with
             * ENODEV, so its indev never empties it */
            fprintf(stderr, "gui_wake: input %d gone\n", fds[i].fd);
            fds[i--] = fds[--nfds];
        } else if (r & POLLIN) {
            woken = WAKE_INPUT;     /* its indev reads it */
        }
    }
    return woken;
}

void gui_wake_read_input(void) {
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        /* Not lv_indev_get_read_timer(): in LVGL 8.3 it reads the indev as a display and returns
         * a field of the wrong structure */
        lv_timer_t *t = i->driver ? i->driver->read_timer : NULL;

        if (t) lv_timer_ready(t);
    }
}

void gui_wake_draw_now(uint32_t min_gap_ms) {
    lv_disp_t *d = lv_disp_get_default();

    if (!d || !d->refr_timer) return;
    uint32_t since = lv_tick_elaps(d->refr_timer->last_run);

    if (since >= min_gap_ms) {
        lv_timer_ready(d->refr_timer);
        draw_pending = false;
    } else {
        /* min_gap_ms after the last frame (a frame since an earlier put-off draw showed that one's input) */
        draw_pending = true;
        draw_after   = d->refr_timer->last_run;
        draw_at      = draw_after + min_gap_ms;
    }
}

/* The refresh timer of the put-off draw, NULL when none waits: a frame since it was put off (the refresh
 * on its own period, in a long pass or after input) drops it */
static lv_timer_t *draw_waiting(void) {
    lv_disp_t *d = lv_disp_get_default();

    if (draw_pending && (!d || !d->refr_timer || d->refr_timer->last_run != draw_after)) draw_pending = false;
    return draw_pending ? d->refr_timer : NULL;
}

void gui_wake_draw_due(void) {
    lv_timer_t *t = draw_waiting();

    if (t && (int32_t)(draw_at - lv_tick_get()) <= 0) {
        lv_timer_ready(t);
        draw_pending = false;
    }
}

int32_t gui_wake_draw_left(uint32_t now) {
    if (!draw_waiting()) return -1;
    int32_t left = (int32_t)(draw_at - now);

    return left > 0 ? left : 0;
}
