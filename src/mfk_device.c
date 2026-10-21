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

#include "screen_light.h"
#include "key_input.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void mfk_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    encoder_t         *knob = drv->user_data;
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
    }
    /* An open dropdown list moves one line a click: the detents of a fast turn, read together,
     * would jump down the list */
    if (sum) {
        lv_obj_t *f = lv_group_get_focused(keyboard_group);

        if (f && lv_obj_check_type(f, &lv_dropdown_class) && lv_dropdown_is_open(f))
            sum = sum > 0 ? 1 : -1;
    }
    data->enc_diff = -sum;      /* the device counts left turns as positive */
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
    lv_indev_drv_init(&knob->indev_drv);
    knob->indev_drv.type      = LV_INDEV_TYPE_ENCODER;
    knob->indev_drv.read_cb   = mfk_read;
    knob->indev_drv.user_data = knob;
    knob->indev = lv_indev_drv_register(&knob->indev_drv);
    lv_indev_set_group(knob->indev, keyboard_group);
    return knob;
}
