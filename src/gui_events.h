/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdint.h>
#include <unistd.h>

/* The radio's buttons (EVENT_KEYPAD, keypad.c) */
typedef enum {
    KEYPAD_UNKNOWN = 0,

    /* front, around the screen */
    KEYPAD_POWER, KEYPAD_GEN, KEYPAD_APP, KEYPAD_KEY, KEYPAD_MSG, KEYPAD_DFN, KEYPAD_DFL,
    KEYPAD_F1, KEYPAD_F2, KEYPAD_F3, KEYPAD_F4, KEYPAD_F5, KEYPAD_LOCK,

    /* top side */
    KEYPAD_PTT, KEYPAD_BAND_DOWN, KEYPAD_BAND_UP, KEYPAD_MODE_AM, KEYPAD_MODE_CW, KEYPAD_MODE_SSB,
    KEYPAD_AB, KEYPAD_PRE, KEYPAD_ATU, KEYPAD_VM, KEYPAD_AGC, KEYPAD_FST
} keypad_key_t;

/* What happened to it: LONG once held for the hold time, then LONG_RELEASE instead of RELEASE */
typedef enum {
    KEYPAD_PRESS = 0, KEYPAD_RELEASE, KEYPAD_LONG, KEYPAD_LONG_RELEASE
} keypad_state_t;

typedef struct {
    keypad_key_t   key;
    keypad_state_t state;
} event_keypad_t;

/* The hand mic keys (EVENT_HKEY, hkey.c). The digits, DOT and CE have the value of what
 * they type; UP and DOWN those of the LVGL arrows */
typedef enum {
    HKEY_CE   = LV_KEY_BACKSPACE,
    HKEY_UP   = LV_KEY_UP,
    HKEY_DOWN = LV_KEY_DOWN,
    HKEY_DOT  = '.',
    HKEY_0    = '0', HKEY_1, HKEY_2, HKEY_3, HKEY_4, HKEY_5, HKEY_6, HKEY_7, HKEY_8, HKEY_9,

    /* the others after KEY_MACRO10 of linux/input.h */
    HKEY_SPCH = 0x299, HKEY_TUNER, HKEY_XFC, HKEY_VM, HKEY_NW, HKEY_F1, HKEY_F2,
    HKEY_MODE, HKEY_FIL, HKEY_GENE, HKEY_FINP,

    HKEY_UNKNOWN,
} hkey_t;

/* As keypad_state_t */
typedef enum {
    HKEY_PRESS = 0, HKEY_RELEASE, HKEY_LONG, HKEY_LONG_RELEASE
} hkey_state_t;

typedef struct {
    hkey_t       key;
    hkey_state_t state;
} event_hkey_t;

/* LVGL keys of the VOL knob (rotary.c, keypad.c), after KEY_MACRO1; the LVGL ones are
 * below 128 */
typedef enum {
    KEY_VOL_LEFT_EDIT = 0x290, KEY_VOL_RIGHT_EDIT,      /* turned while it changes a value */
    KEY_VOL_LEFT_SELECT, KEY_VOL_RIGHT_SELECT,          /* turned while it chooses the control */
    KEY_VOL_PRESS,          /* not ESC: a menu or an app does not close on it */
} keys_t;

/* Event codes of the GUI, registered with LVGL by event_init() */
extern uint32_t EVENT_ROTARY;           /* main knob, rotary_data_t */
extern uint32_t EVENT_KEYPAD;           /* event_keypad_t */
extern uint32_t EVENT_HKEY;             /* event_hkey_t */
extern uint32_t EVENT_RADIO_TX;
extern uint32_t EVENT_RADIO_RX;
extern uint32_t EVENT_SCREEN_UPDATE;
extern uint32_t EVENT_MSG_UPDATE;
extern uint32_t EVENT_GPS;
extern uint32_t EVENT_BAND_UP;
extern uint32_t EVENT_BAND_DOWN;

void event_init();

/* The GUI loop: deliver what event_send() queued */
void event_obj_check();

/* Forget the queued events of deleted objects */
void event_obj_drop_deleted();

/* From any thread: an event to obj, delivered by the GUI loop; param (malloc'ed or NULL)
 * is freed after. A full queue drops it */
void event_send(lv_obj_t *obj, lv_event_code_t event_code, void *param);

/* A key to the focused widget, as event_send() */
void event_send_key(int32_t key);
