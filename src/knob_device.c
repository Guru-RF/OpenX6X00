/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The VOL knob and the main knob. VOL turns become LVGL keys, one press and release per
 * detent, the key chosen by the knob's state (edit or select). The main knob goes to the
 * screen as EVENT_ROTARY with its speed. */

#include "knob_device.h"

#include "screen_light.h"
#include "key_input.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* VOL: what is still to send of the last read, and whether the last key sent was down */
static int32_t pending;
static bool    key_down;

static void vol_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    rotary_t *knob = drv->user_data;

    if (pending == 0) {
        struct input_event in;
        int32_t            sum = 0;
        bool               turned = false;

        while (read(knob->fd, &in, sizeof(in)) > 0) {
            if (in.type == EV_REL) {
                sum += in.value;
                turned = true;
            }
        }
        if (turned) {
            backlight_tick();
            pending                = sum;
            data->continue_reading = true;
        }
    } else if (key_down) {
        data->state            = LV_INDEV_STATE_RELEASED;
        data->continue_reading = true;
    } else {
        int32_t step = pending > 0 ? 1 : -1;     /* positive counts are turns to the left */

        pending -= step;
        data->state            = LV_INDEV_STATE_PRESSED;
        data->key              = step > 0 ? knob->left[knob->state] : knob->right[knob->state];
        data->continue_reading = true;
    }
    key_down = data->state == LV_INDEV_STATE_PRESSED;
}

rotary_t *rotary_init(char *dev_name) {
    int fd = open(dev_name, O_RDWR | O_NOCTTY | O_NDELAY);

    if (fd == -1) {
        LV_LOG_ERROR("unable to open rotary interface:");
        return NULL;
    }
    fcntl(fd, F_SETFL, O_ASYNC | O_NONBLOCK);

    rotary_t *knob = calloc(1, sizeof(*knob));

    knob->fd = fd;
    lv_indev_drv_init(&knob->indev_drv);
    knob->indev_drv.type      = LV_INDEV_TYPE_KEYPAD;
    knob->indev_drv.read_cb   = vol_read;
    knob->indev_drv.user_data = knob;
    knob->indev = lv_indev_drv_register(&knob->indev_drv);
    lv_indev_set_group(knob->indev, keyboard_group);
    return knob;
}

/* Main knob: a thread waits for the detents and timestamps them as they come, so the
 * speed (ms per detent) does not depend on how busy the GUI loop is (upstream ae2af22) */

#define TURNS_MAX 16

static struct {
    int             fd;
    rotary_data_t   turns[TURNS_MAX];     /* a ring: the thread writes, the GUI reads */
    uint8_t         head, tail;
    pthread_mutex_t lock;
    lv_indev_drv_t  drv;
} knob_main = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void *main_knob_thread(void *arg) {
    struct pollfd      pfd = {.fd = knob_main.fd, .events = POLLIN};
    struct input_event in;
    struct timespec    last = {0, 0}, now;

    for (;;) {
        int16_t detents = 0;

        if (poll(&pfd, 1, -1) <= 0) {
            continue;
        }
        while (read(knob_main.fd, &in, sizeof(in)) > 0) {
            if (in.type == EV_REL) {
                detents += in.value;
            }
        }
        if (!detents) {
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);

        float ms = (now.tv_sec - last.tv_sec) * 1000.0f + (now.tv_nsec - last.tv_nsec) * 1e-6f;

        last = now;
        ms   = LV_MIN(ms, 500.0f) / LV_ABS(detents);

        pthread_mutex_lock(&knob_main.lock);
        uint8_t next = (knob_main.head + 1) % TURNS_MAX;

        if (next != knob_main.tail) {     /* full: dropped, the GUI is stuck anyway */
            knob_main.turns[knob_main.head] = (rotary_data_t){.diff = detents, .dt = LV_MAX((uint16_t)ms, 1)};
            knob_main.head                  = next;
        }
        pthread_mutex_unlock(&knob_main.lock);
    }
    return NULL;
}

/* Everything queued since the last GUI loop becomes one event, so a fast turn moves the
 * frequency once per frame: the detents summed, the time per detent averaged over them.
 * A change of direction ends the batch */
static void main_knob_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int32_t  sum = 0;
    uint32_t count = 0, ms = 0;

    pthread_mutex_lock(&knob_main.lock);
    while (knob_main.tail != knob_main.head) {
        rotary_data_t *t = &knob_main.turns[knob_main.tail];

        if (sum && (t->diff > 0) != (sum > 0)) {
            data->continue_reading = true;
            break;
        }
        sum   += t->diff;
        count += LV_ABS(t->diff);
        ms    += t->dt * LV_ABS(t->diff);
        knob_main.tail = (knob_main.tail + 1) % TURNS_MAX;
    }
    pthread_mutex_unlock(&knob_main.lock);

    if (sum) {
        rotary_data_t *ev = malloc(sizeof(*ev));

        backlight_tick();
        ev->diff = sum;
        ev->dt   = LV_MAX(ms / count, 1);
        lv_event_send(lv_scr_act(), EVENT_ROTARY, ev);
    }
}

void rotary_main_init(char *dev_name) {
    pthread_t thread;

    knob_main.fd = open(dev_name, O_RDWR | O_NOCTTY | O_NDELAY);
    if (knob_main.fd == -1) {
        LV_LOG_ERROR("unable to open main knob");
        return;
    }
    fcntl(knob_main.fd, F_SETFL, O_NONBLOCK);
    pthread_create(&thread, NULL, main_knob_thread, NULL);
    pthread_detach(thread);

    lv_indev_drv_init(&knob_main.drv);
    knob_main.drv.type    = LV_INDEV_TYPE_KEYPAD;
    knob_main.drv.read_cb = main_knob_read;
    lv_indev_set_group(lv_indev_drv_register(&knob_main.drv), keyboard_group);
}
