/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200 behind hw.h: Aether Radio's X6200 control library (the base over its
 *  serial link and I2C, the GPIO pins), the base's register layout and its packet. The X6200's
 *  base computes the spectrum itself and sends 512 dB bins instead of IQ samples (HW_CAP_PSD),
 *  and it reports the POWER key in the packet. Built from hw/x6100/hw_x6100.c.
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU (the flow decoding, the init sequence)
 *  Copyright (c) 2026 RF.Guru
 */

#define _GNU_SOURCE
#include "../hw.h"
#include "../../base_guard.h"

#include <aether_radio/x6200_control/control.h>
#include <aether_radio/x6200_control/low/flow.h>
#include <aether_radio/x6200_control/low/gpio.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The GUI's enums (radio_types.h) are the library's numbers, except the modes: the X6200 has
 * SAM between AM and NFM (lib_mode() below) */
#define SAME(a, b) _Static_assert((int)(a) == (int)(b), #a " is not " #b)
SAME(RADIO_VFO_A, X6200_VFO_A);          SAME(RADIO_VFO_B, X6200_VFO_B);
SAME(radio_mode_lsb, x6200_mode_lsb);    SAME(radio_mode_lsb_dig, x6200_mode_lsb_dig);
SAME(radio_mode_usb, x6200_mode_usb);    SAME(radio_mode_usb_dig, x6200_mode_usb_dig);
SAME(radio_mode_cw, x6200_mode_cw);      SAME(radio_mode_cwr, x6200_mode_cwr);
SAME(radio_mode_am, x6200_mode_am);
SAME(radio_agc_off, x6200_agc_off);      SAME(radio_agc_slow, x6200_agc_slow);
SAME(radio_agc_fast, x6200_agc_fast);    SAME(radio_agc_auto, x6200_agc_auto);
SAME(radio_att_off, x6200_att_off);      SAME(radio_att_on, x6200_att_on);
SAME(radio_pre_off, x6200_pre_off);      SAME(radio_pre_on, x6200_pre_on);
SAME(radio_key_manual, x6200_key_manual); SAME(radio_key_auto_left, x6200_key_auto_left);
SAME(radio_key_auto_right, x6200_key_auto_right);
SAME(radio_iambic_a, x6200_iambic_a);    SAME(radio_iambic_b, x6200_iambic_b);
SAME(radio_mic_builtin, x6200_mic_builtin); SAME(radio_mic_handle, x6200_mic_handle);
SAME(radio_mic_auto, x6200_mic_auto);
SAME(RADIO_HKEY_SPCH, X6200_HKEY_SPCH);  SAME(RADIO_HKEY_TUNER, X6200_HKEY_TUNER);
SAME(RADIO_HKEY_XFC, X6200_HKEY_XFC);    SAME(RADIO_HKEY_UP, X6200_HKEY_UP);
SAME(RADIO_HKEY_DOWN, X6200_HKEY_DOWN);  SAME(RADIO_HKEY_VM, X6200_HKEY_VM);
SAME(RADIO_HKEY_NW, X6200_HKEY_NW);      SAME(RADIO_HKEY_F1, X6200_HKEY_F1);
SAME(RADIO_HKEY_F2, X6200_HKEY_F2);      SAME(RADIO_HKEY_1, X6200_HKEY_1);
SAME(RADIO_HKEY_2, X6200_HKEY_2);        SAME(RADIO_HKEY_3, X6200_HKEY_3);
SAME(RADIO_HKEY_4, X6200_HKEY_4);        SAME(RADIO_HKEY_5, X6200_HKEY_5);
SAME(RADIO_HKEY_6, X6200_HKEY_6);        SAME(RADIO_HKEY_7, X6200_HKEY_7);
SAME(RADIO_HKEY_8, X6200_HKEY_8);        SAME(RADIO_HKEY_9, X6200_HKEY_9);
SAME(RADIO_HKEY_DOT, X6200_HKEY_DOT);    SAME(RADIO_HKEY_0, X6200_HKEY_0);
SAME(RADIO_HKEY_CE, X6200_HKEY_CE);      SAME(RADIO_HKEY_MODE, X6200_HKEY_MODE);
SAME(RADIO_HKEY_FIL, X6200_HKEY_FIL);    SAME(RADIO_HKEY_GENE, X6200_HKEY_GENE);
SAME(RADIO_HKEY_FINP, X6200_HKEY_FINP);
#undef SAME

/* The GUI's NFM is the base's 8 (SAM, the GUI has no mode for yet, is its 7) */
static x6200_mode_t lib_mode(radio_mode_t m) {
    return m == radio_mode_nfm ? x6200_mode_nfm : (x6200_mode_t)m;
}

/* Bits of x6200_sple_atue_trx that key the radio from the GUI */
#define TX_BITS (x6200_iptt | x6200_modem_trx | x6200_atu_tune | x6200_swrscan_trx | x6200_calibration_trx)

#define PSD_BINS    512     /* dB bins in a packet, the lowest frequency first */
#define PSD_OFFSET  126.0f  /* bin value 126 is 0 dBm: the stock X6200 GUI's constant */
#define PSD_SPAN_HZ 384000  /* the bins at decimation 0; halved per step (the stock GUI's FFT_FULL_WIDTH) */
#define PSD_MAX_DEC 3       /* the stock GUI goes no further (48 kHz); what the base does beyond is unknown */
#define SMETER_CORR 4       /* dB the stock GUI adds to the base's S-meter reading */

static x6200_flow_t *pack;
static float         psd_db[PSD_BINS];
static uint8_t       fft_dec;       /* the base's spectrum decimation as last set */
static char          ver_str[32] = "?";
static radio_base_ver_t ver;

/* The notch: the GUI sets on/off and auto apart, the base takes one mode */
static bool dnf_on, dnf_auto;

/* ---- The library's writes, watched (base_guard.h) ---------------------------------------- */

#define TRIES       3
#define RETRY_US    2000

static bool (*lib_cmd)(x6200_cmd_enum_t cmd, uint32_t arg);

__attribute__((constructor)) static void find_lib_cmd(void) {
    lib_cmd = (bool (*)(x6200_cmd_enum_t, uint32_t))dlsym(RTLD_NEXT, "x6200_control_cmd");
    if (!lib_cmd) {
        fprintf(stderr, "hw_x6200: x6200_control_cmd not found in the control library\n");
        abort();
    }
}

/* The library's own name: its setters call it through the PLT, so every write lands here (see
 * hw_x6100.c and base_guard.h for the why) */
bool x6200_control_cmd(x6200_cmd_enum_t cmd, uint32_t arg) {
    uint32_t prev = x6200_control_get(cmd);
    int      fails = 0;
    bool     ok;

    while (!(ok = lib_cmd(cmd, arg)) && ++fails < TRIES) {
        usleep(RETRY_US);
    }
    if (fails) {
        base_guard_write_failed(cmd, ok);
    }
    if (cmd == x6200_sple_atue_trx && (prev & TX_BITS) && !(arg & TX_BITS)) {
        base_guard_unkeyed();
    }
    return ok;
}

/* ---- Init ----------------------------------------------------------------------------- */

bool hw_init(void) {
    if (!x6200_gpio_init()) {
        return false;
    }
    x6200_gpio_set(x6200_pin_morse_key, 1);     /* Morse key off */
    if (!x6200_flow_init()) {
        return false;
    }
    pack = malloc(sizeof(x6200_flow_t));
    return pack != NULL;
}

bool hw_control_init(void) {
    return x6200_control_init();
}

void hw_bb_reset(void) {
    x6200_gpio_set(x6200_pin_bb_reset, 1);
    usleep(100000);
    x6200_gpio_set(x6200_pin_bb_reset, 0);
}

/* The base's info block: the library hands back its first 128 bytes; the firmware version is
 * the string at 0x41 (its own init prints it as "BASE fw version"). Kept as major.minor.patch
 * for the GUI, revision 0: the X6200's capabilities do not depend on it */
void hw_start(void) {
    const char *info = x6200_control_get_fw_version();
    const char *s    = info ? info + 0x41 : NULL;

    memset(&ver, 0, sizeof(ver));
    if (s && *s) {
        snprintf(ver_str, sizeof(ver_str), "%s", s);
        while (*s && !isdigit((unsigned char)*s)) {
            s++;
        }
        unsigned a = 0, b = 0, c = 0;
        if (sscanf(s, "%u.%u.%u", &a, &b, &c) >= 2) {
            ver = (radio_base_ver_t){a, b, c, 0};
        }
    }
}

radio_base_ver_t hw_base_ver(void) {
    return ver;
}

const char *hw_base_ver_str(void) {
    return ver_str;
}

/* The base narrows its spectrum for the zoom, finds the notch's carrier itself, and sends the
 * spectrum ready-made. No IF shift */
uint32_t hw_caps(void) {
    return HW_CAP_ZOOM | HW_CAP_DNF_AUTO | HW_CAP_PSD;
}

/* ---- Settings ------------------------------------------------------------------------- */

/* The settings with a register of their own: compared before the write, read back by hw_get_i() */
static int reg_of(hw_param_t p) {
    switch (p) {
        case HW_RIT:            return x6200_rit;
        case HW_XIT:            return x6200_xit;
        case HW_AGC_TIME:       return x6200_agctime;
        case HW_ATU_NETWORK:    return x6200_atu_network;
        default:                return -1;
    }
}

int32_t hw_get_i(hw_param_t p) {
    int      reg = reg_of(p);
    uint32_t f;

    switch (p) {
        case HW_FILTER1_LOW:
            f = x6200_control_get(x6200_rxfilter);
            return (int16_t)(f >> 16);
        case HW_FILTER1_HIGH:
            f = x6200_control_get(x6200_rxfilter);
            return (int16_t)(f & 0xFFFF);
        default:
            return reg < 0 ? 0 : (int32_t)x6200_control_get((x6200_cmd_enum_t)reg);
    }
}

static void dnf_apply(void) {
    x6200_control_dnf_set(!dnf_on ? x6200_dnf_off : dnf_auto ? x6200_dnf_auto : x6200_dnf_manual);
}

bool hw_set_i(hw_param_t p, int32_t v) {
    int reg = reg_of(p);

    if (reg >= 0) {
        if (x6200_control_get((x6200_cmd_enum_t)reg) == (uint32_t)v) {
            return false;
        }
        x6200_control_cmd((x6200_cmd_enum_t)reg, (uint32_t)v);
        return true;
    }
    switch (p) {
        case HW_VFO_CUR:        x6200_control_vfo_set((x6200_vfo_t)v); break;
        case HW_SPLIT:          x6200_control_split_set(v); break;
        case HW_RFG:            x6200_control_rfg_set(v); break;
        /* One squelch setting in the GUI, two on the base (voice and FM): both follow it */
        case HW_SQL:            x6200_control_sql_set(v); x6200_control_sql_fm_set(v); break;
        case HW_RXVOL:          x6200_control_rxvol_set(v); break;
        case HW_ATU_ENABLED:    x6200_control_atu_set(v); break;
        case HW_ATU_TUNE:       x6200_control_atu_tune(v); break;
        case HW_SWRSCAN:        x6200_control_swrscan_set(v); break;
        case HW_PTT:            x6200_control_ptt_set(v); break;
        case HW_MODEM:          x6200_control_modem_set(v); break;
        case HW_RECORD:         x6200_control_record_set(v); break;
        case HW_MIC:            x6200_control_mic_set((x6200_mic_sel_t)v); break;
        case HW_HMIC:           x6200_control_hmic_set(v); break;
        case HW_IMIC:           x6200_control_imic_set(v); break;
        case HW_MONI:           x6200_control_monitor_level_set(v); break;
        case HW_LINE_IN:        x6200_control_linein_set(v); break;
        case HW_LINE_OUT:       x6200_control_lineout_set(v); break;
        case HW_SPMODE:         x6200_control_spmode_set(v); break;
        case HW_CHARGER:        x6200_control_charger_set(v); break;
        case HW_BIAS_DRIVE:     x6200_control_bias_drive_set(v); break;
        case HW_BIAS_FINAL:     x6200_control_bias_final_set(v); break;
        /* The GUI asks for 100 kHz over 2^v; the base's steps are 384 kHz over 2^dec: one step
         * further than asked keeps the stream wider than the screen (192 kHz over 2^v), the GUI
         * takes the middle of it (hw_pack_t.psd_span) */
        case HW_FFT_DEC:
            fft_dec = v + 1 > PSD_MAX_DEC ? PSD_MAX_DEC : v + 1;
            x6200_control_fft_dec_set(fft_dec);
            break;
        /* One RX filter register: low and high halves. The second filter (the X6100's wider one
         * around the first) does not exist here */
        case HW_FILTER1_LOW:
            if (hw_get_i(HW_FILTER1_LOW) == v) return false;
            x6200_control_rx_filter_set_low(v);
            break;
        case HW_FILTER1_HIGH:
            if (hw_get_i(HW_FILTER1_HIGH) == v) return false;
            x6200_control_rx_filter_set_high(v);
            break;
        case HW_COMP_ON:        x6200_control_comp_set(v); break;
        case HW_COMP_LEVEL:     x6200_control_comp_level_set((x6200_comp_level_t)v); break;
        case HW_VOX:            x6200_control_vox_set(v); break;
        case HW_VOX_GAIN:       x6200_control_vox_gain_set(v); break;
        case HW_VOX_AG:         x6200_control_vox_ag_set(v); break;
        case HW_VOX_DELAY:      x6200_control_vox_delay_set(v); break;
        case HW_KEY_TONE:       x6200_control_key_tone_set(v); break;
        case HW_KEY_SPEED:      x6200_control_key_speed_set(v); break;
        case HW_KEY_MODE:       x6200_control_key_mode_set((x6200_key_mode_t)v); break;
        case HW_IAMBIC_MODE:    x6200_control_iambic_mode_set((x6200_iambic_mode_t)v); break;
        case HW_KEY_VOL:        x6200_control_key_vol_set(v); break;
        case HW_KEY_TRAIN:      x6200_control_key_train_set(v); break;
        case HW_QSK_TIME:       x6200_control_qsk_time_set(v); break;
        case HW_AGC_HANG:       x6200_control_agc_hang_set(v); break;
        case HW_AGC_KNEE:       x6200_control_agc_knee_set(v); break;
        case HW_AGC_SLOPE:      x6200_control_agc_slope_set(v); break;
        case HW_DNF:            dnf_on = v; dnf_apply(); break;
        case HW_DNF_AUTO:       dnf_auto = v; dnf_apply(); break;
        case HW_DNF_CENTER:     x6200_control_dnf_center_set(v); break;
        case HW_DNF_WIDTH:      x6200_control_dnf_width_set(v); break;
        case HW_NB:             x6200_control_nb_set(v); break;
        case HW_NB_LEVEL:       x6200_control_nb_level_set(v); break;
        case HW_NB_WIDTH:       x6200_control_nb_width_set(v); break;
        case HW_NR:             x6200_control_nr_set(v); break;
        case HW_NR_LEVEL:       x6200_control_nr_level_set(v); break;
        /* The X6100 patched firmware's extras: this base has no register for them */
        case HW_TX_I_OFFSET:
        case HW_TX_Q_OFFSET:
        case HW_FILTER2_LOW:
        case HW_FILTER2_HIGH:
        case HW_IF_SHIFT_ON:
        case HW_IF_SHIFT_HZ:
        case HW_FM_EMP:
        case HW_TX_FILTER_LOW:
        case HW_TX_FILTER_HIGH:
        case HW_CESSB:
        case HW_CW_PEAK:
        case HW_CW_PEAK_Q:
            return false;
        default:
            fprintf(stderr, "hw_x6200: hw_set_i(%d) is not an integer setting\n", p);
            return false;
    }
    return true;
}

bool hw_set_f(hw_param_t p, float v) {
    switch (p) {
        case HW_TXPWR_F:            x6200_control_txpwr_set(v); break;
        case HW_KEY_RATIO_F:        x6200_control_key_ratio_set(v); break;
        /* Not on this base */
        case HW_ADC_DAC_GAIN_F:
        case HW_DAC_GAIN_F:
        case HW_COMP_THRESHOLD_F:
        case HW_COMP_MAKEUP_F:
        case HW_CESSB_POWER_UP_F:
            return false;
        default:
            fprintf(stderr, "hw_x6200: hw_set_f(%d) is not a float setting\n", p);
            return false;
    }
    return true;
}

bool hw_vfo_set_freq(radio_vfo_t vfo, int32_t hz) {
    if (x6200_control_get(vfo == RADIO_VFO_A ? x6200_vfoa_freq : x6200_vfob_freq) == (uint32_t)hz) {
        return false;
    }
    x6200_control_vfo_freq_set((x6200_vfo_t)vfo, hz);
    return true;
}

bool hw_vfo_set_mode(radio_vfo_t vfo, radio_mode_t mode) {
    x6200_mode_t m = lib_mode(mode);

    if (x6200_control_get(vfo == RADIO_VFO_A ? x6200_vfoa_mode : x6200_vfob_mode) == (uint32_t)m) {
        return false;
    }
    x6200_control_vfo_mode_set((x6200_vfo_t)vfo, m);
    return true;
}

void hw_vfo_set_agc(radio_vfo_t vfo, radio_agc_t agc) {
    x6200_control_vfo_agc_set((x6200_vfo_t)vfo, (x6200_agc_t)agc);
}

void hw_vfo_set_att(radio_vfo_t vfo, bool on) {
    x6200_control_vfo_att_set((x6200_vfo_t)vfo, on ? x6200_att_on : x6200_att_off);
}

void hw_vfo_set_pre(radio_vfo_t vfo, bool on) {
    x6200_control_vfo_pre_set((x6200_vfo_t)vfo, on ? x6200_pre_on : x6200_pre_off);
}

void hw_poweroff(void) {
    x6200_control_poweroff();
}

/* The library reopens the bus and tries once more on its own, and reports nothing but errno */
bool hw_resend_all(void) {
    errno = 0;
    x6200_control_idle();
    return errno == 0;
}

bool hw_tx_keyed_by_gui(void) {
    return (x6200_control_get(x6200_sple_atue_trx) & TX_BITS) != 0;
}

void hw_pin_set(hw_pin_t pin, bool on) {
    switch (pin) {
        case HW_PIN_WIFI:       x6200_gpio_set(x6200_pin_wifi, on ? 1 : 0); break;
        case HW_PIN_LIGHT:      x6200_gpio_set(x6200_pin_light, on ? 1 : 0); break;
        case HW_PIN_USB_CAT:    x6200_gpio_set(x6200_pin_usb, on ? 1 : 0); break;
        case HW_PIN_MORSE_KEY:  x6200_gpio_set(x6200_pin_morse_key, on ? 0 : 1); break;  /* active low */
        case HW_PIN_BB_RESET:   x6200_gpio_set(x6200_pin_bb_reset, on ? 1 : 0); break;
    }
}

/* ---- The packet stream -------------------------------------------------------------------- */

/* 552 bytes every ~35 ms: 512 spectrum bins as unsigned dB (126 = 0 dBm), the flags, the
 * meters (dbm is the S-meter, negated), the hand mic's key */
bool hw_flow_read(hw_pack_t *p) {
    if (!x6200_flow_read(pack)) {
        return false;
    }
    for (size_t i = 0; i < PSD_BINS; i++) {
        psd_db[i] = (float)pack->samples[i] - PSD_OFFSET;
    }
    p->samples    = NULL;
    p->n_samples  = 0;
    p->psd        = psd_db;
    p->n_psd      = PSD_BINS;
    p->psd_span   = PSD_SPAN_HZ >> fft_dec;
    p->smeter_dbm = -(int16_t)pack->dbm + SMETER_CORR;
    p->lo_freq    = 0;
    p->fft_dec    = 0;
    p->vary_freq  = false;
    p->tx         = pack->flag.tx;
    p->atu_status = pack->flag.atu_status;
    p->charging   = pack->flag.charging;
    p->vext_flag  = pack->flag.vext;
    p->power_key  = pack->flag.power_key;
    p->tx_power   = pack->tx_power;
    p->vswr       = pack->vswr;
    p->alc_level  = pack->alc_level;
    p->vext       = pack->vext;
    p->vbat       = pack->vbat;
    p->batcap     = pack->batcap;
    p->atu_params = pack->atu_params;
    p->hkey       = pack->hkey;
    return true;
}

void hw_flow_restart(void) {
    x6200_flow_restart();
}
