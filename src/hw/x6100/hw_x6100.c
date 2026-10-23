/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6100 behind hw.h: Aether Radio's control library (the base over its
 *  serial link, the IQ stream, the GPIO pins), the base's register layout and its packet.
 *  What used to be spread through radio.c.
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU (the flow decoding, the init sequence)
 *  Copyright (c) 2026 RF.Guru
 */

#define _GNU_SOURCE
#include "../hw.h"
#include "../../base_guard.h"

#include <aether_radio/x6100_control/control.h>
#include <aether_radio/x6100_control/low/flow.h>
#include <aether_radio/x6100_control/low/gpio.h>

#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The GUI's enums (radio_types.h) are the library's numbers: handed straight to it below */
#define SAME(a, b) _Static_assert((int)(a) == (int)(b), #a " is not " #b)
SAME(RADIO_VFO_A, X6100_VFO_A);          SAME(RADIO_VFO_B, X6100_VFO_B);
SAME(radio_mode_lsb, x6100_mode_lsb);    SAME(radio_mode_lsb_dig, x6100_mode_lsb_dig);
SAME(radio_mode_usb, x6100_mode_usb);    SAME(radio_mode_usb_dig, x6100_mode_usb_dig);
SAME(radio_mode_cw, x6100_mode_cw);      SAME(radio_mode_cwr, x6100_mode_cwr);
SAME(radio_mode_am, x6100_mode_am);      SAME(radio_mode_nfm, x6100_mode_nfm);
SAME(radio_agc_off, x6100_agc_off);      SAME(radio_agc_slow, x6100_agc_slow);
SAME(radio_agc_fast, x6100_agc_fast);    SAME(radio_agc_auto, x6100_agc_auto);
SAME(radio_att_off, x6100_att_off);      SAME(radio_att_on, x6100_att_on);
SAME(radio_pre_off, x6100_pre_off);      SAME(radio_pre_on, x6100_pre_on);
SAME(radio_key_manual, x6100_key_manual); SAME(radio_key_auto_left, x6100_key_auto_left);
SAME(radio_key_auto_right, x6100_key_auto_right);
SAME(radio_iambic_a, x6100_iambic_a);    SAME(radio_iambic_b, x6100_iambic_b);
SAME(radio_mic_builtin, x6100_mic_builtin); SAME(radio_mic_handle, x6100_mic_handle);
SAME(radio_mic_auto, x6100_mic_auto);
SAME(RADIO_HKEY_SPCH, X6100_HKEY_SPCH);  SAME(RADIO_HKEY_TUNER, X6100_HKEY_TUNER);
SAME(RADIO_HKEY_XFC, X6100_HKEY_XFC);    SAME(RADIO_HKEY_UP, X6100_HKEY_UP);
SAME(RADIO_HKEY_DOWN, X6100_HKEY_DOWN);  SAME(RADIO_HKEY_VM, X6100_HKEY_VM);
SAME(RADIO_HKEY_NW, X6100_HKEY_NW);      SAME(RADIO_HKEY_F1, X6100_HKEY_F1);
SAME(RADIO_HKEY_F2, X6100_HKEY_F2);      SAME(RADIO_HKEY_1, X6100_HKEY_1);
SAME(RADIO_HKEY_2, X6100_HKEY_2);        SAME(RADIO_HKEY_3, X6100_HKEY_3);
SAME(RADIO_HKEY_4, X6100_HKEY_4);        SAME(RADIO_HKEY_5, X6100_HKEY_5);
SAME(RADIO_HKEY_6, X6100_HKEY_6);        SAME(RADIO_HKEY_7, X6100_HKEY_7);
SAME(RADIO_HKEY_8, X6100_HKEY_8);        SAME(RADIO_HKEY_9, X6100_HKEY_9);
SAME(RADIO_HKEY_DOT, X6100_HKEY_DOT);    SAME(RADIO_HKEY_0, X6100_HKEY_0);
SAME(RADIO_HKEY_CE, X6100_HKEY_CE);      SAME(RADIO_HKEY_MODE, X6100_HKEY_MODE);
SAME(RADIO_HKEY_FIL, X6100_HKEY_FIL);    SAME(RADIO_HKEY_GENE, X6100_HKEY_GENE);
SAME(RADIO_HKEY_FINP, X6100_HKEY_FINP);
#undef SAME

/* Bits of x6100_sple_atue_trx that key the radio from the GUI */
#define TX_BITS (x6100_iptt | x6100_modem_trx | x6100_atu_tune | x6100_swrscan_trx | x6100_calibration_trx)

#define SAMPLES     512     /* complex samples in a packet, twice as many in the bf16 format */

typedef enum {
    flow_fp32 = 0,
    flow_bf16,
} flow_fmt_t;

static x6100_flow_t    *pack;
static float _Complex   samples_buf[SAMPLES * 2];
static x6100_base_ver_t base_ver;
static bool             new_base;   /* rev 8 and up: the bf16 stream, the centre frequency in the packet */

/* ---- The library's writes, watched (base_guard.h) ---------------------------------------- */

#define TRIES       3
#define RETRY_US    2000

static bool (*lib_cmd)(x6100_cmd_enum_t cmd, uint32_t arg);

__attribute__((constructor)) static void find_lib_cmd(void) {
    lib_cmd = (bool (*)(x6100_cmd_enum_t, uint32_t))dlsym(RTLD_NEXT, "x6100_control_cmd");
    if (!lib_cmd) {
        fprintf(stderr, "hw_x6100: x6100_control_cmd not found in the control library\n");
        abort();
    }
}

/* The library's own name: its setters (PTT, modem, tune, frequency...) call it through the PLT,
 * so every write lands here. The library updates its register copy before writing and its
 * setters drop the result: a write that fails is sent again here, and radio.c resends the
 * whole block (the error found by Hany El Imam, base_guard.h) */
bool x6100_control_cmd(x6100_cmd_enum_t cmd, uint32_t arg) {
    uint32_t prev = x6100_control_get(cmd);
    int      fails = 0;
    bool     ok;

    while (!(ok = lib_cmd(cmd, arg)) && ++fails < TRIES) {
        usleep(RETRY_US);
    }
    if (fails) {
        base_guard_write_failed(cmd, ok);
    }
    if (cmd == x6100_sple_atue_trx && (prev & TX_BITS) && !(arg & TX_BITS)) {
        base_guard_unkeyed();
    }
    return ok;
}

/* ---- Init ----------------------------------------------------------------------------- */

bool hw_init(void) {
    if (!x6100_gpio_init()) {
        return false;
    }
    x6100_gpio_set(x6100_pin_morse_key, 1);     /* Morse key off */
    if (!x6100_flow_init()) {
        return false;
    }
    pack = malloc(sizeof(x6100_flow_t));
    return pack != NULL;
}

bool hw_control_init(void) {
    return x6100_control_init();
}

void hw_bb_reset(void) {
    x6100_gpio_set(x6100_pin_bb_reset, 1);
    usleep(100000);
    x6100_gpio_set(x6100_pin_bb_reset, 0);
}

static int32_t ver_cmp(x6100_base_ver_t a, x6100_base_ver_t b) {
    int32_t d;
    if ((d = (int32_t)a.major - b.major)) return d;
    if ((d = (int32_t)a.minor - b.minor)) return d;
    if ((d = (int32_t)a.patch - b.patch)) return d;
    return (int32_t)a.rev - b.rev;
}

void hw_start(void) {
    base_ver = x6100_control_get_base_ver();
    new_base = base_ver.rev >= 8;
    /* Centre mode by default on the patched bases (the old behaviour) */
    if (ver_cmp(base_ver, (x6100_base_ver_t){1, 1, 9, 0}) >= 0 || new_base) {
        x6100_control_if_shift_set(false);
    }
    if (new_base) {
        x6100_control_bf16_flow_set(true);
    }
}

radio_base_ver_t hw_base_ver(void) {
    x6100_base_ver_t v = x6100_control_get_base_ver();
    return (radio_base_ver_t){v.major, v.minor, v.patch, v.rev};
}

const char *hw_base_ver_str(void) {
    return x6100_control_get_fw_version_str();
}

/* The patched firmware (1.1.9, and every rev 8 board) zooms and shifts the IF itself; the auto
 * notch, the compressor and the codec gain came with rev 3, the band gain and FM emphasis with
 * rev 8, the TX filter with rev 9, CESSB with rev 12 */
uint32_t hw_caps(void) {
    x6100_base_ver_t v    = x6100_control_get_base_ver();
    uint32_t         caps = HW_CAP_TX_IQ_OFFSET;

    if (ver_cmp(v, (x6100_base_ver_t){1, 1, 9, 0}) >= 0 || v.rev >= 8) {
        caps |= HW_CAP_ZOOM | HW_CAP_IF_SHIFT;
    }
    if (v.rev >= 3) {
        caps |= HW_CAP_DNF_AUTO | HW_CAP_COMP | HW_CAP_COMP_SHAPE | HW_CAP_CODEC_GAIN | HW_CAP_TX_LEVEL;
    }
    if (v.rev >= 8) {
        caps |= HW_CAP_BAND_GAIN | HW_CAP_FM_EMP;
    }
    if (v.rev >= 9) {
        caps |= HW_CAP_TX_FILTER;
    }
    if (v.rev >= 12) {
        caps |= HW_CAP_CESSB;
    }
    return caps;
}

/* ---- Settings ------------------------------------------------------------------------- */

/* The settings with a register of their own: compared before the write, read back by hw_get_i() */
static int reg_of(hw_param_t p) {
    switch (p) {
        case HW_TX_I_OFFSET:    return x6100_txiofs;
        case HW_TX_Q_OFFSET:    return x6100_txqofs;
        case HW_RIT:            return x6100_rit;
        case HW_XIT:            return x6100_xit;
        case HW_AGC_TIME:       return x6100_agctime;
        case HW_FILTER1_LOW:    return x6100_filter1_low;
        case HW_FILTER1_HIGH:   return x6100_filter1_high;
        case HW_FILTER2_LOW:    return x6100_filter2_low;
        case HW_FILTER2_HIGH:   return x6100_filter2_high;
        case HW_ATU_NETWORK:    return x6100_atu_network;
        default:                return -1;
    }
}

int32_t hw_get_i(hw_param_t p) {
    int reg = reg_of(p);
    return reg < 0 ? 0 : (int32_t)x6100_control_get((x6100_cmd_enum_t)reg);
}

bool hw_set_i(hw_param_t p, int32_t v) {
    int reg = reg_of(p);

    if (reg >= 0) {
        if (x6100_control_get((x6100_cmd_enum_t)reg) == (uint32_t)v) {
            return false;
        }
        x6100_control_cmd((x6100_cmd_enum_t)reg, (uint32_t)v);
        return true;
    }
    switch (p) {
        case HW_VFO_CUR:        x6100_control_vfo_set((x6100_vfo_t)v); break;
        case HW_SPLIT:          x6100_control_split_set(v); break;
        case HW_RFG:            x6100_control_rfg_set(v); break;
        case HW_SQL:            x6100_control_sql_set(v); break;
        case HW_RXVOL:          x6100_control_rxvol_set(v); break;
        case HW_ATU_ENABLED:    x6100_control_atu_set(v); break;
        case HW_ATU_TUNE:       x6100_control_atu_tune(v); break;
        case HW_SWRSCAN:        x6100_control_swrscan_set(v); break;
        case HW_PTT:            x6100_control_ptt_set(v); break;
        case HW_MODEM:          x6100_control_modem_set(v); break;
        case HW_RECORD:         x6100_control_record_set(v); break;
        case HW_MIC:            x6100_control_mic_set((x6100_mic_sel_t)v); break;
        case HW_HMIC:           x6100_control_hmic_set(v); break;
        case HW_IMIC:           x6100_control_imic_set(v); break;
        case HW_MONI:           x6100_control_moni_set(v); break;
        case HW_LINE_IN:        x6100_control_linein_set(v); break;
        case HW_LINE_OUT:       x6100_control_lineout_set(v); break;
        case HW_SPMODE:         x6100_control_spmode_set(v); break;
        case HW_CHARGER:        x6100_control_charger_set(v); break;
        case HW_BIAS_DRIVE:     x6100_control_bias_drive_set(v); break;
        case HW_BIAS_FINAL:     x6100_control_bias_final_set(v); break;
        case HW_IF_SHIFT_ON:    x6100_control_if_shift_set(v); break;
        case HW_IF_SHIFT_HZ:    x6100_control_if_shift_freq_set(v); break;
        case HW_FFT_DEC:        x6100_control_fftdec_set(v); break;
        case HW_FM_EMP:         x6100_control_fm_emp(v); break;
        case HW_TX_FILTER_LOW:  x6100_control_tx_filter_low_set(v); break;
        case HW_TX_FILTER_HIGH: x6100_control_tx_filter_high_set(v); break;
        case HW_COMP_ON:        x6100_control_comp_set(v); break;
        case HW_COMP_LEVEL:     x6100_control_comp_level_set((x6100_comp_level_t)v); break;
        case HW_CESSB:          x6100_control_cessb_set(v); break;
        case HW_VOX:            x6100_control_vox_set(v); break;
        case HW_VOX_GAIN:       x6100_control_vox_gain_set(v); break;
        case HW_VOX_AG:         x6100_control_vox_ag_set(v); break;
        case HW_VOX_DELAY:      x6100_control_vox_delay_set(v); break;
        case HW_KEY_TONE:       x6100_control_key_tone_set(v); break;
        case HW_KEY_SPEED:      x6100_control_key_speed_set(v); break;
        case HW_KEY_MODE:       x6100_control_key_mode_set((x6100_key_mode_t)v); break;
        case HW_IAMBIC_MODE:    x6100_control_iambic_mode_set((x6100_iambic_mode_t)v); break;
        case HW_KEY_VOL:        x6100_control_key_vol_set(v); break;
        case HW_KEY_TRAIN:      x6100_control_key_train_set(v); break;
        case HW_QSK_TIME:       x6100_control_qsk_time_set(v); break;
        case HW_CW_PEAK:        x6100_control_cw_peak_set(v); break;
        case HW_CW_PEAK_Q:      x6100_control_cw_peak_q_set(v); break;
        case HW_AGC_HANG:       x6100_control_agc_hang_set(v); break;
        case HW_AGC_KNEE:       x6100_control_agc_knee_set(v); break;
        case HW_AGC_SLOPE:      x6100_control_agc_slope_set(v); break;
        case HW_DNF:            x6100_control_dnf_set(v); break;
        case HW_DNF_CENTER:     x6100_control_dnf_center_set(v); break;
        case HW_DNF_WIDTH:      x6100_control_dnf_width_set(v); break;
        case HW_DNF_AUTO:       x6100_control_dnf_update_set(v); break;
        case HW_NB:             x6100_control_nb_set(v); break;
        case HW_NB_LEVEL:       x6100_control_nb_level_set(v); break;
        case HW_NB_WIDTH:       x6100_control_nb_width_set(v); break;
        case HW_NR:             x6100_control_nr_set(v); break;
        case HW_NR_LEVEL:       x6100_control_nr_level_set(v); break;
        default:
            fprintf(stderr, "hw_x6100: hw_set_i(%d) is not an integer setting\n", p);
            return false;
    }
    return true;
}

bool hw_set_f(hw_param_t p, float v) {
    switch (p) {
        case HW_TXPWR_F:            x6100_control_txpwr_set(v); break;
        case HW_ADC_DAC_GAIN_F:     x6100_control_adc_dac_gain_set(v); break;
        case HW_DAC_GAIN_F:         x6100_control_dac_gain_set(v); break;
        case HW_COMP_THRESHOLD_F:   x6100_control_comp_threshold_set(v); break;
        case HW_COMP_MAKEUP_F:      x6100_control_comp_makeup_set(v); break;
        case HW_CESSB_POWER_UP_F:   x6100_control_cessb_power_up_set(v); break;
        case HW_KEY_RATIO_F:        x6100_control_key_ratio_set(v); break;
        default:
            fprintf(stderr, "hw_x6100: hw_set_f(%d) is not a float setting\n", p);
            return false;
    }
    return true;
}

bool hw_vfo_set_freq(radio_vfo_t vfo, int32_t hz) {
    if (x6100_control_get(vfo == RADIO_VFO_A ? x6100_vfoa_freq : x6100_vfob_freq) == (uint32_t)hz) {
        return false;
    }
    x6100_control_vfo_freq_set((x6100_vfo_t)vfo, hz);
    return true;
}

bool hw_vfo_set_mode(radio_vfo_t vfo, radio_mode_t mode) {
    /* No SAM or WFM on this base: AM and NFM */
    if (mode == radio_mode_sam) mode = radio_mode_am;
    if (mode == radio_mode_wfm) mode = radio_mode_nfm;
    if (x6100_control_get(vfo == RADIO_VFO_A ? x6100_vfoa_mode : x6100_vfob_mode) == (uint32_t)mode) {
        return false;
    }
    x6100_control_vfo_mode_set((x6100_vfo_t)vfo, (x6100_mode_t)mode);
    return true;
}

void hw_vfo_set_agc(radio_vfo_t vfo, radio_agc_t agc) {
    x6100_control_vfo_agc_set((x6100_vfo_t)vfo, (x6100_agc_t)agc);
}

void hw_vfo_set_att(radio_vfo_t vfo, bool on) {
    x6100_control_vfo_att_set((x6100_vfo_t)vfo, on ? x6100_att_on : x6100_att_off);
}

void hw_vfo_set_pre(radio_vfo_t vfo, bool on) {
    x6100_control_vfo_pre_set((x6100_vfo_t)vfo, on ? x6100_pre_on : x6100_pre_off);
}

void hw_poweroff(void) {
    x6100_control_poweroff();
}

/* The library reopens the bus and tries once more on its own, and reports nothing but errno */
bool hw_resend_all(void) {
    errno = 0;
    x6100_control_idle();
    return errno == 0;
}

bool hw_tx_keyed_by_gui(void) {
    return (x6100_control_get(x6100_sple_atue_trx) & TX_BITS) != 0;
}

void hw_pin_set(hw_pin_t pin, bool on) {
    switch (pin) {
        case HW_PIN_WIFI:       x6100_gpio_set(x6100_pin_wifi, on ? 1 : 0); break;
        case HW_PIN_LIGHT:      x6100_gpio_set(x6100_pin_light, on ? 1 : 0); break;
        case HW_PIN_USB_CAT:    x6100_gpio_set(x6100_pin_usb, on ? 1 : 0); break;
        case HW_PIN_MORSE_KEY:  x6100_gpio_set(x6100_pin_morse_key, on ? 0 : 1); break;  /* active low */
        case HW_PIN_BB_RESET:   x6100_gpio_set(x6100_pin_bb_reset, on ? 1 : 0); break;
    }
}

/* ---- The packet stream -------------------------------------------------------------------- */

bool hw_flow_read(hw_pack_t *p) {
    if (!x6100_flow_read(pack)) {
        return false;
    }
    flow_info_t   info = pack->flow_info;
    float _Complex *flow_samples = (float _Complex *)((char *)pack + offsetof(x6100_flow_t, samples));

    p->samples   = flow_samples;
    p->n_samples = SAMPLES;
    p->lo_freq   = 0;
    p->fft_dec   = 0;
    /*
    On patched:
        RX:
            SSB: lo_freq = fg_freq - if_shift + rit
            CW: lo_freq = fg_freq - key_tone - if_shift + rit
            CWR: lo_freq = fg_freq + key_tone - if_shift + rit
            AM: lo_freq = fg_freq - 100 + rit
            FM: lo_freq = fg_freq - 200 + rit
        TX:
            SSB: lo_freq = fg_freq + xit
            CW: lo_freq = fg_freq + xit
        On OEM: lo_freq = 0
    */
    if (new_base) {
        p->lo_freq = info.lo_freq;
        p->fft_dec = 1U << info.fft_dec;
        if (info.flow_fmt == flow_bf16) {
            /* bf16: the high half of each float, twice as many samples */
            uint16_t *in  = (uint16_t *)flow_samples;
            uint32_t *out = (uint32_t *)samples_buf;
            for (size_t i = 0; i < SAMPLES * 2 * 2; i++) {
                out[i] = (uint32_t)in[i] << 16;
            }
            p->samples   = samples_buf;
            p->n_samples = SAMPLES * 2;
        }
    }
    p->tx         = pack->flag.tx;
    p->atu_status = pack->flag.atu_status;
    p->charging   = pack->flag.charging;
    p->vext_flag  = pack->flag.vext;
    p->tx_power   = pack->tx_power;
    p->vswr       = pack->vswr;
    p->alc_level  = pack->alc_level;
    p->vext       = pack->vext;
    p->vbat       = pack->vbat;
    p->batcap     = pack->batcap;
    p->atu_params = pack->atu_params;
    p->vary_freq  = info.vary_freq;
    p->hkey       = pack->hkey;
    p->psd        = NULL;
    p->n_psd      = 0;
    p->psd_span   = 0;
    p->smeter_dbm = 0;
    p->power_key  = false;
    return true;
}

void hw_flow_restart(void) {
    x6100_flow_restart();
}
