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

#include <stdint.h>

/* The base's (the baseband's) firmware version as it reports it */
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint8_t rev;
} radio_base_ver_t;

/* What the radio's base can do, radio_caps() (hw_caps() of the backend): the GUI sends and
 * shows only what applies. The X6100's depend on its firmware; the X6200's are fixed */
typedef enum {
    HW_CAP_ZOOM     = 1 << 0,   /* the base narrows its stream for the zoom (HW_FFT_DEC) */
    HW_CAP_IF_SHIFT = 1 << 1,   /* HW_IF_SHIFT_ON / HW_IF_SHIFT_HZ */
    HW_CAP_DNF_AUTO = 1 << 2,   /* the notch finds the carrier itself (HW_DNF_AUTO) */
    HW_CAP_PSD      = 1 << 3,   /* the base sends its own spectrum, no IQ samples (hw_pack_t.psd) */
    /* The transmitter's settings (Settings > TX CAL, Audio) */
    HW_CAP_TX_IQ_OFFSET = 1 << 4,   /* HW_TX_I_OFFSET / HW_TX_Q_OFFSET */
    HW_CAP_BAND_GAIN    = 1 << 5,   /* HW_DAC_GAIN_F per band */
    HW_CAP_CODEC_GAIN   = 1 << 6,   /* HW_ADC_DAC_GAIN_F */
    HW_CAP_CESSB        = 1 << 7,   /* HW_CESSB, HW_CESSB_POWER_UP_F */
    HW_CAP_COMP         = 1 << 8,   /* HW_COMP_ON, HW_COMP_LEVEL */
    HW_CAP_COMP_SHAPE   = 1 << 9,   /* HW_COMP_THRESHOLD_F, HW_COMP_MAKEUP_F */
    HW_CAP_TX_FILTER    = 1 << 10,  /* HW_TX_FILTER_LOW / HIGH */
    HW_CAP_FM_EMP       = 1 << 11,  /* HW_FM_EMP */
    HW_CAP_TX_LEVEL     = 1 << 12,  /* the base sets the TX power itself: digital audio goes at one level */
    HW_CAP_SAM_WFM      = 1 << 13,  /* the modes radio_mode_sam and radio_mode_wfm */
    HW_CAP_KEEPALIVE    = 1 << 14,  /* the register block every second in every state, TX included: the X6200's
                                     * base fades its RF out about a second into a TX the host is silent in */
    HW_CAP_TX_NO_ALC    = 1 << 15,  /* the ALC does not follow the drive (the X6200: about 0 from 0.03 to 5 W): digital
                                     * drive held once the power reaches the setting */
} hw_cap_t;

/* The hand mic's keys as the base reports them (mic_keys.c) */
#define RADIO_HKEY_SPCH     0xC1
#define RADIO_HKEY_TUNER    0xC2
#define RADIO_HKEY_XFC      0xC4
#define RADIO_HKEY_UP       0x84
#define RADIO_HKEY_DOWN     0x04
#define RADIO_HKEY_VM       0x81
#define RADIO_HKEY_NW       0x82
#define RADIO_HKEY_F1       0x48
#define RADIO_HKEY_F2       0x44
#define RADIO_HKEY_1        0xE1
#define RADIO_HKEY_2        0xE2
#define RADIO_HKEY_3        0xE4
#define RADIO_HKEY_4        0x61
#define RADIO_HKEY_5        0x62
#define RADIO_HKEY_6        0x64
#define RADIO_HKEY_7        0xA1
#define RADIO_HKEY_8        0xA2
#define RADIO_HKEY_9        0xA4
#define RADIO_HKEY_DOT      0x21
#define RADIO_HKEY_0        0x22
#define RADIO_HKEY_CE       0x24
#define RADIO_HKEY_MODE     0xE8
#define RADIO_HKEY_FIL      0x68
#define RADIO_HKEY_GENE     0xA8
#define RADIO_HKEY_FINP     0x28

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
    radio_mode_nfm     = 7,
    radio_mode_sam     = 8,     /* synchronous AM and wide FM: the X6200 (HW_CAP_SAM_WFM); a base */
    radio_mode_wfm     = 9,     /* without them takes AM and NFM for them */
    radio_mode_last    = radio_mode_wfm
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
