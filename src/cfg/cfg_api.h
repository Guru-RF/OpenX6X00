#pragma once

// C-compatible API for the SettingsManager's parameters (Stage B). This header
// is the umbrella entry point for C (and C++) consumers: it aggregates the
// per-type C-API headers and exposes the opaque extern parameter pointers,
// filled by cfg_api_init().
//
// Per-type APIs (all included here for convenience):
//   - subject_api.h            generic Subject/SubjectT/Observer helpers
//   - parameter_api.h          Parameter<T> typed get/set
//   - computed_api.h           ComputedParameter<T> set/get
//   - atu_cache_api.h          ATU tuner-network cache
//   - settings_manager_api.h   init, flush, band/VFO switching, freq helpers
//
// All accessors route through Parameter<T>::set / SubjectT<T>::get, so
// validators, deferred-write enqueue and observer notifications all apply.
// Ownership: the parameters (and the SettingsManager singleton) are static and
// owned by C++ (cfg_api.cpp). C code only receives/holds opaque pointers and
// must never free them. Observers returned by *_subscribe are owned by C/UI
// code and freed with param_unsubscribe.
//
// cfg_api_init() does NOT open the DB or call cfg_db_init(): the caller owns
// the sqlite3 connection and table initialisation (avoids double-Init).

#include "computed_api.h"
#include "subject_api.h"
#include "parameter_api.h"
#include "atu_cache_api.h"
#include "settings_manager_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Opaque extern globals, filled by cfg_api_init() ---
// Each maps to a concrete public SettingsManager member. Int params map to
// ParamInt, scaled-float params to ParamFloat (read-only), and p_encoder_bind (a string)
// to ParamText. The extern list is the exhaustive set of C-reachable
// parameters — every SettingsManager parameter is exposed so migrating C/C++
// consumers can read/write the same values through one API.
//
// Grouped by storage scope: GLOBAL -> BAND -> MODE -> OTHER (Transverter) ->
// COMPUTED (current operating state, not persisted).

// --- GLOBAL params (flat `params` table) ---
extern ParamInt         *cfg_volume;            // p_volume
extern ParamInt         *cfg_squelch;           // p_squelch
extern ParamInt         *cfg_rfgain;            // p_rfgain
extern ParamInt         *cfg_rit;               // p_rit
extern ParamInt         *cfg_xit;               // p_xit
extern ParamFloat       *cfg_pwr;               // p_pwr
extern ParamInt         *cfg_band_id;           // p_band_id (persisted current band)
extern ParamInt         *cfg_mic;               // p_mic
extern ParamInt         *cfg_wefax_align;       // p_wefax_align, WEFAX picture shift
extern ParamInt         *cfg_wefax_tilt;        // p_wefax_tilt, WEFAX picture slant
extern ParamInt         *cfg_hmic;              // p_hmic
extern ParamInt         *cfg_imic;              // p_imic
extern ParamInt         *cfg_moni;              // p_moni
extern ParamInt         *cfg_atu_enabled;       // p_atu_enabled

// UI
extern ParamInt         *cfg_auto_level_enabled; // p_auto_level_enabled
extern ParamFloat       *cfg_auto_level_offset;  // p_auto_level_offset
extern ParamInt         *cfg_knob_info;          // p_knob_info
extern ParamInt         *cfg_spectrum_show;      // p_spectrum_show
extern ParamInt         *cfg_spectrum_height;    // p_spectrum_height
extern ParamInt         *cfg_digi_text_compact;  // p_digi_text_compact
extern ParamInt         *cfg_cluster_spots_wf;   // p_cluster_spots_wf
extern ParamInt         *cfg_walkie;             // p_walkie
extern ParamInt         *cfg_walkie_mem;         // p_walkie_mem
extern ParamInt         *cfg_walkie_ch;          // p_walkie_ch
extern ParamInt         *cfg_walkie_mic;         // p_walkie_mic
extern ParamInt         *cfg_band_plan;          // p_band_plan
extern ParamInt         *cfg_smeter_needle;      // p_smeter_needle
extern ParamInt         *cfg_smeter_color;       // p_smeter_color
extern ParamInt         *cfg_wf_palette;         // p_wf_palette
extern ParamInt         *cfg_filter_color;       // p_filter_color
extern ParamInt         *cfg_spectrum_line_color; // p_spectrum_line_color
extern ParamInt         *cfg_spectrum_fill_color; // p_spectrum_fill_color
extern ParamInt         *cfg_spectrum_fill_gradient; // p_spectrum_fill_gradient
extern ParamInt         *cfg_smeter_bar_color;   // p_smeter_bar_color
extern ParamInt         *cfg_swr_color;          // p_swr_color
extern ParamInt         *cfg_swr_protect;        // p_swr_protect
extern ParamInt         *cfg_smeter_lamp;        // p_smeter_lamp
extern ParamInt         *cfg_broadcast_name;     // p_broadcast_name
extern ParamInt         *cfg_cluster_column;     // p_cluster_column
extern ParamInt         *cfg_cluster_order;      // p_cluster_order
extern ParamInt         *cfg_cluster_all_bands;  // p_cluster_all_bands
extern ParamInt         *cfg_waterfall_ref;      // p_waterfall_ref
extern ParamInt         *cfg_smeter_cal;         // p_band_smeter_cal (current band)
extern ParamInt         *cfg_waterfall_speed;    // p_waterfall_speed
extern ParamInt         *cfg_wf_mode;            // p_wf_mode
extern ParamText        *cfg_encoder_bind;       // p_encoder_bind

// VOX
extern ParamInt *cfg_vox_on;   // p_vox_en
extern ParamInt *cfg_vox_gain; // p_vox_gain
extern ParamInt *cfg_vox_ag;   // p_vox_ag
extern ParamInt *cfg_vox_delay; // p_vox_delay

// FT8
extern ParamInt *cfg_ft8_show_all;   // p_ft8_show_all
extern ParamInt *cfg_ft8_protocol;   // p_ft8_protocol
extern ParamInt *cfg_ft8_auto;       // p_ft8_auto
extern ParamInt *cfg_ft8_hold_freq;  // p_ft8_hold_freq
extern ParamInt *cfg_ft8_foryou;     // p_ft8_foryou
extern ParamInt *cfg_wspr_tx_pct;    // p_wspr_tx_pct
extern ParamInt *cfg_spot_snap_hold; // p_spot_snap_hold
extern ParamInt *cfg_spot_snap_slow; // p_spot_snap_slow
extern ParamInt *cfg_freq_accel_onset; // p_freq_accel_onset
extern ParamInt *cfg_wspr_tx_freq;   // p_wspr_tx_freq
extern ParamInt *cfg_wspr_upload;    // p_wspr_upload
extern ParamInt *cfg_pskreporter;    // p_pskreporter
extern ParamInt *cfg_field_boot;     // p_field_boot
extern ParamInt *cfg_field_respot;   // p_field_respot
extern ParamInt *cfg_voice_msg_gain; // p_voice_msg_gain
extern ParamInt *cfg_ft8_max_repeats; // p_ft8_max_repeats
extern ParamInt *cfg_ft8_info;       // p_ft8_info

// SWR scan
extern ParamInt *cfg_swrscan_linear; // p_swrscan_linear
extern ParamInt *cfg_swrscan_span;   // p_swrscan_span

// CW
extern ParamInt   *cfg_key_tone;            // p_key_tone
extern ParamInt   *cfg_key_speed;           // p_key_speed
extern ParamInt   *cfg_key_mode;            // p_key_mode
extern ParamInt   *cfg_iambic_mode;         // p_iambic_mode
extern ParamInt   *cfg_key_vol;             // p_key_vol
extern ParamInt   *cfg_key_train;           // p_key_train
extern ParamInt   *cfg_qsk_time;            // p_qsk_time
extern ParamFloat *cfg_key_ratio;           // p_key_ratio
extern ParamInt   *cfg_cw_peak_on;          // p_cw_peak_on
extern ParamInt   *cfg_cw_peak_q;           // p_cw_peak_q
extern ParamInt   *cfg_paddle_map[4];       // p_paddle_kbd, _btn, _midi, _ser (paddle_src_t order)
extern ParamInt   *cfg_paddle_swap;         // p_paddle_swap
extern ParamInt   *cfg_paddle_serial;       // p_paddle_serial

// CW decoder
extern ParamInt   *cfg_cwt_level;           // p_cwt_level
extern ParamInt   *cfg_cwt_group;           // p_cwt_group
extern ParamInt   *cfg_cw_decoder;         // p_cw_decoder
extern ParamInt   *cfg_cw_tune;            // p_cw_tune
extern ParamInt   *cfg_cw_live;            // p_cw_live
extern ParamFloat *cfg_cw_decoder_snr;     // p_cw_decoder_snr

// AGC
extern ParamInt *cfg_agc_hang;  // p_agc_hang
extern ParamInt *cfg_agc_knee;  // p_agc_knee
extern ParamInt *cfg_agc_slope; // p_agc_slope

// DSP
extern ParamInt *cfg_dnf;        // p_dnf
extern ParamInt *cfg_dnf_center; // p_dnf_center
extern ParamInt *cfg_dnf_width;  // p_dnf_width
extern ParamInt *cfg_dnf_auto;   // p_dnf_auto
extern ParamInt *cfg_nb;         // p_nb
extern ParamInt *cfg_nb_level;   // p_nb_level
extern ParamInt *cfg_nb_width;   // p_nb_width
extern ParamInt *cfg_nr;         // p_nr
extern ParamInt *cfg_nr_level;   // p_nr_level

// DSP custom
extern ParamFloat *cfg_output_gain;          // p_output_gain
extern ParamInt   *cfg_comp;                 // p_comp
extern ParamFloat *cfg_comp_threshold_offset; // p_comp_threshold_offset
extern ParamFloat *cfg_comp_makeup_offset;    // p_comp_makeup_offset
extern ParamInt   *cfg_fm_emphasis;          // p_fm_emphasis
extern ParamInt   *cfg_tx_filter_low;        // p_tx_filter_low
extern ParamInt   *cfg_tx_filter_high;       // p_tx_filter_high
extern ParamInt   *cfg_cessb_on;             // p_cessb_on
extern ParamFloat *cfg_cessb_power_up;       // p_cessb_power_up

// --- BAND params ---
extern ParamInt   *cfg_band_current_vfo;  // p_band_current_vfo
extern ParamInt   *cfg_band_if_shift;     // p_band_if_shift
extern ParamInt   *cfg_band_vfoa_freq;    // p_band_vfoa_freq
extern ParamInt   *cfg_band_vfob_freq;    // p_band_vfob_freq
extern ParamFloat *cfg_band_dac_offset;   // p_band_dac_offset
extern ParamInt   *cfg_band_grid_min;     // p_band_grid_min
extern ParamInt   *cfg_band_grid_max;     // p_band_grid_max
extern ParamInt   *cfg_band_split;        // p_band_split
extern ParamInt   *cfg_band_tx_i_offset;  // p_band_tx_i_offset
extern ParamInt   *cfg_band_tx_q_offset;  // p_band_tx_q_offset
extern ParamInt   *cfg_band_vfoa_mode;    // p_band_vfoa_mode
extern ParamInt   *cfg_band_vfob_mode;    // p_band_vfob_mode
extern ParamInt   *cfg_band_vfoa_att;     // p_band_vfoa_att
extern ParamInt   *cfg_band_vfob_att;     // p_band_vfob_att
extern ParamInt   *cfg_band_vfoa_pre;     // p_band_vfoa_pre
extern ParamInt   *cfg_band_vfob_pre;     // p_band_vfob_pre
extern ParamInt   *cfg_band_vfoa_agc;     // p_band_vfoa_agc
extern ParamInt   *cfg_band_vfob_agc;     // p_band_vfob_agc

// --- MODE params ---
extern ParamInt *cfg_mode_squelch;   // p_mode_squelch
extern ParamInt *cfg_mode_zoom;      // p_mode_zoom ("spectrum_factor")
extern ParamInt *cfg_mode_freq_step; // p_mode_freq_step

// --- Transverter params (OTHER storage, fixed HW conversion) ---
// Values are Hz (p_transverter_{0,1}_{from,to,shift}).
extern ParamInt *cfg_transverter_0_from;
extern ParamInt *cfg_transverter_0_to;
extern ParamInt *cfg_transverter_0_shift;
extern ParamInt *cfg_transverter_1_from;
extern ParamInt *cfg_transverter_1_to;
extern ParamInt *cfg_transverter_1_shift;

// --- Computed params (current operating state, not persisted) ---
extern ComputedParamInt *cfg_fg_freq;          // cp_fg_freq
extern ComputedParamInt *cfg_cur_mode;         // cp_cur_mode
extern ComputedParamInt *cfg_cur_agc;          // cp_cur_agc
extern ComputedParamInt *cfg_cur_att;          // cp_cur_att
extern ComputedParamInt *cfg_cur_pre;          // cp_cur_pre
extern ComputedParamInt *cfg_bg_freq;          // cp_bg_freq
extern ComputedParamInt *cfg_cur_filter_low;   // cp_cur_filter_low
extern ComputedParamInt *cfg_cur_filter_high;  // cp_cur_filter_high
extern ComputedParamInt *cfg_cur_filter_bw;    // cp_cur_filter_bw
extern ComputedParamInt *cfg_mode_lo_offset;   // cp_mode_lo_offset

// S-meter calibration for the site ping: *start = the value of bands never calibrated, then up to
// max bands whose own value differs from it (name, dB). Returns how many
int cfg_smeter_cal_bands(int *start, char (*names)[16], int *vals, int max);

#ifdef __cplusplus
} // extern "C"
#endif
