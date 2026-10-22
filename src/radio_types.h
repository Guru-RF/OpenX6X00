/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the radio's vocabulary as the GUI speaks it: modes, VFOs, AGC, the key.
 *  These are the GUI's own; radio.c maps them onto the control library of the radio
 *  it is built for (the X6100's uses the same numbers, checked there). The numbers are
 *  what the settings database, CAT, TCI and the browser panel carry: do not renumber.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

typedef enum {
    RADIO_VFO_A = 0,
    RADIO_VFO_B
} radio_vfo_t;

typedef enum {
    radio_mode_lsb     = 0,
    radio_mode_lsb_dig = 1,
    radio_mode_usb     = 2,
    radio_mode_usb_dig = 3,
    radio_mode_cw      = 4,
    radio_mode_cwr     = 5,
    radio_mode_am      = 6,
    radio_mode_nfm     = 7
} radio_mode_t;

typedef enum {
    radio_agc_off  = 0,
    radio_agc_slow = 1,
    radio_agc_fast = 2,
    radio_agc_auto = 3
} radio_agc_t;

typedef enum {
    radio_att_off = 0,
    radio_att_on  = 1
} radio_att_t;

typedef enum {
    radio_pre_off = 0,
    radio_pre_on  = 1
} radio_pre_t;

/* The CW key: straight, or the paddle with the dot on the left or the right */
typedef enum {
    radio_key_manual     = 0,
    radio_key_auto_left  = 1,
    radio_key_auto_right = 2
} radio_key_mode_t;

typedef enum {
    radio_iambic_a = 0,
    radio_iambic_b = 1
} radio_iambic_mode_t;

typedef enum {
    radio_mic_builtin = 0,
    radio_mic_handle,
    radio_mic_auto
} radio_mic_sel_t;
