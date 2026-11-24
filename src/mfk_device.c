/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The MFK knob as an LVGL encoder: its turns from its own device, its press from the
 * buttons' device (keypad.c sets pressed) */

#include "mfk_device.h"

#include "cfg/cfg_api.h"
#include "gui_wake.h"
#include "screen_light.h"
#include "key_input.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* The rings' devices (the X6100 has the outer one only), the counts short of a click each keeps, the setting as the
 * last read saw it, and the turns a browser sent while Swapped (mfk_rings.h) */
static int        ring_fd[MFK_RINGS] = {-1, -1};
static int32_t    ring_rest[MFK_RINGS];
static bool       ring_swapped;
static atomic_int ring_injected;

void mfk_rings_inner(int fd) {
    ring_fd[MFK_RING_INNER] = fd;
}

bool mfk_rings_swapped(void) {
    return ring_fd[MFK_RING_INNER] >= 0 && cfg_mfk_rings && param_i_get(cfg_mfk_rings) != 0;
}

/* The setting is read here, at each read: a change of it counts from the next click on */
int32_t mfk_rings_read(mfk_role_t role, bool *turned) {
    bool               swapped = mfk_rings_swapped();
    mfk_ring_t         ring = mfk_ring_of(role, swapped);
    struct input_event in;
    int32_t            counts = 0, clicks;

    if (swapped != ring_swapped) {      /* part of a click of the one use is not part of a click of the other */
        ring_swapped = swapped;
        ring_rest[MFK_RING_OUTER] = ring_rest[MFK_RING_INNER] = 0;
    }
    while (ring_fd[ring] >= 0 && read(ring_fd[ring], &in, sizeof(in)) > 0) {
        if (in.type == EV_REL) {
            counts += in.value;
            *turned = true;
        }
    }
    clicks = mfk_ring_clicks(ring, role, counts, &ring_rest[ring]);
    if (role == MFK_ROLE_KNOB) {
        int n = atomic_exchange(&ring_injected, 0);

        if (n) {
            clicks -= n;
            *turned = true;
        }
    }
    return clicks;
}

/* Stock: a browser's MFK turns go into the outer ring's device, as always (remote_server.c). Swapped: that device
 * steps the control now, and what the inner ring's device takes when written to is not known, so they come here */
void mfk_rings_inject(int n) {
    atomic_fetch_add(&ring_injected, n);
    gui_wake_input();
}

static void mfk_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    encoder_t *knob = drv->user_data;
    bool       turned = false;
    int32_t    sum = mfk_rings_read(MFK_ROLE_KNOB, &turned);

    if (turned && backlight_knob_tick()) {
        sum = 0;    /* the turn that ends the screensaver only ends it */
    }
    /* An open dropdown list moves one line a click: the detents of a fast turn, read together,
     * would jump down the list */
    if (sum) {
        lv_obj_t *f = lv_group_get_focused(keyboard_group);

        if (f && lv_obj_check_type(f, &lv_dropdown_class) && lv_dropdown_is_open(f))
            sum = sum > 0 ? 1 : -1;
    }
    data->enc_diff = -sum;      /* left turns are positive (mfk_rings.h) */
    data->state    = knob->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

encoder_t *encoder_init(char *dev_name) {
    int fd = open(dev_name, O_RDWR | O_NOCTTY | O_NDELAY);

    if (fd == -1) {
        perror("unable to open encoder interface:");
        return NULL;
    }
    fcntl(fd, F_SETFL, O_ASYNC | O_NONBLOCK);

    encoder_t *knob = calloc(1, sizeof(*knob));

    knob->fd = fd;
    ring_fd[MFK_RING_OUTER] = fd;
    lv_indev_drv_init(&knob->indev_drv);
    knob->indev_drv.type      = LV_INDEV_TYPE_ENCODER;
    knob->indev_drv.read_cb   = mfk_read;
    knob->indev_drv.user_data = knob;
    knob->indev = lv_indev_drv_register(&knob->indev_drv);
    lv_indev_set_group(knob->indev, keyboard_group);
    return knob;
}
