/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The radio's buttons (front and top side, the knob presses) from their input devices.
 * A button goes to the screen as EVENT_KEYPAD: press, release, and after the hold time
 * long and its release. The VOL press is an LVGL key of its own (KEY_VOL_PRESS); the MFK
 * press is the button of the MFK encoder (encoder.c). */

#include "front_keys.h"

#include "screen_light.h"
#include "key_input.h"
#include "main.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define HOLD_MS 1000

static const struct {
    uint16_t     code;
    keypad_key_t key;
} buttons[] = {
    /* front */
    {KEY_POWER, KEYPAD_POWER},          {BTN_TRIGGER_HAPPY1, KEYPAD_GEN},  {BTN_TRIGGER_HAPPY7, KEYPAD_APP},
    {BTN_TRIGGER_HAPPY2, KEYPAD_KEY},   {BTN_TRIGGER_HAPPY8, KEYPAD_MSG},  {BTN_TRIGGER_HAPPY3, KEYPAD_DFN},
    {BTN_TRIGGER_HAPPY9, KEYPAD_DFL},   {BTN_TRIGGER_HAPPY13, KEYPAD_F1},  {BTN_TRIGGER_HAPPY14, KEYPAD_F2},
    {BTN_TRIGGER_HAPPY15, KEYPAD_F3},   {BTN_TRIGGER_HAPPY19, KEYPAD_F4},  {BTN_TRIGGER_HAPPY20, KEYPAD_F5},
    {BTN_TRIGGER_HAPPY25, KEYPAD_LOCK},
    /* top side */
    {BTN_TRIGGER_HAPPY4, KEYPAD_PTT},   {BTN_TRIGGER_HAPPY5, KEYPAD_BAND_DOWN},
    {BTN_TRIGGER_HAPPY6, KEYPAD_BAND_UP}, {BTN_TRIGGER_HAPPY10, KEYPAD_MODE_AM},
    {BTN_TRIGGER_HAPPY11, KEYPAD_MODE_CW}, {BTN_TRIGGER_HAPPY12, KEYPAD_MODE_SSB},
    {BTN_TRIGGER_HAPPY16, KEYPAD_AB},   {BTN_TRIGGER_HAPPY17, KEYPAD_PRE}, {BTN_TRIGGER_HAPPY18, KEYPAD_ATU},
    {BTN_TRIGGER_HAPPY22, KEYPAD_VM},   {BTN_TRIGGER_HAPPY23, KEYPAD_AGC}, {BTN_TRIGGER_HAPPY24, KEYPAD_FST},
};

#define VOL_PRESS BTN_TRIGGER_HAPPY21
#define MFK_PRESS BTN_TRIGGER_HAPPY27

static event_keypad_t cur;          /* sent by address: the receivers read it at once */
static lv_timer_t    *hold_timer;
static uint32_t       hold_ms = HOLD_MS;

void keypad_set_long_time(uint32_t hold) {
    hold_ms = hold ? hold : HOLD_MS;
}

static void send(keypad_state_t state) {
    cur.state = state;
    lv_event_send(lv_scr_act(), EVENT_KEYPAD, &cur);
}

static void held_cb(lv_timer_t *t) {
    send(KEYPAD_LONG);
    hold_timer = NULL;      /* one shot: LVGL deletes it */
}

static keypad_key_t button_of(uint16_t code) {
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        if (buttons[i].code == code) {
            return buttons[i].key;
        }
    }
    LV_LOG_WARN("Unknown key");
    return KEYPAD_UNKNOWN;
}

/* One input event a call; value 1 is a press, anything else (release, autorepeat) lets go */
static void button_event(const struct input_event *in) {
    cur.key = button_of(in->code);
    if (hold_timer) {
        lv_timer_del(hold_timer);
        hold_timer = NULL;
    }
    if (in->value == 1) {
        send(KEYPAD_PRESS);
        hold_timer = lv_timer_create(held_cb, hold_ms, NULL);
        lv_timer_set_repeat_count(hold_timer, 1);
    } else if (cur.state == KEYPAD_PRESS) {
        send(KEYPAD_RELEASE);
    } else if (cur.state == KEYPAD_LONG) {
        send(KEYPAD_LONG_RELEASE);
    }
}

static void keypad_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    keypad_t          *pad = drv->user_data;
    struct input_event in;

    if (read(pad->fd, &in, sizeof(in)) > 0) {
        /* All the queued events in one pass: one per pass took 4 GUI loops for a key
         * (press, sync, release, sync), 100 ms and more while the screen is busy */
        data->continue_reading = true;

        if (in.type == EV_KEY) {
            backlight_tick();
            if (in.code == VOL_PRESS) {
                pad->evdev_key   = KEY_VOL_PRESS;
                pad->evdev_state = in.value ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            } else if (in.code == MFK_PRESS) {
                mfk->pressed = in.value != 0;
                /* The encoder samples it at its next read: a quick press and release
                 * in the same pass would be lost */
                data->continue_reading = false;
            } else {
                button_event(&in);
            }
        }
    }
    data->key   = pad->evdev_key;
    data->state = pad->evdev_state;
}

keypad_t *keypad_init(char *dev_name) {
    int fd = open(dev_name, O_RDWR | O_NOCTTY | O_NDELAY);

    if (fd == -1) {
        perror("unable to open keypad interface:");
        return NULL;
    }
    fcntl(fd, F_SETFL, O_ASYNC | O_NONBLOCK);

    keypad_t *pad = calloc(1, sizeof(*pad));

    pad->fd          = fd;
    pad->evdev_state = LV_INDEV_STATE_RELEASED;
    lv_indev_drv_init(&pad->indev_drv);
    pad->indev_drv.type            = LV_INDEV_TYPE_KEYPAD;
    pad->indev_drv.read_cb         = keypad_read;
    pad->indev_drv.user_data       = pad;
    pad->indev_drv.long_press_time = HOLD_MS;
    pad->indev = lv_indev_drv_register(&pad->indev_drv);
    lv_indev_set_group(pad->indev, keyboard_group);
    return pad;
}
