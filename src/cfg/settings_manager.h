#pragma once

// SettingsManager: orchestrator that owns the global/band/mode parameters and
// the deferred-write queue (PendingWrites), and implements band/mode switching
// semantics.
//
// Scope of this header (project rules):
//   - Only src/cfg/ dependencies: parameter.h, pending_writes.h,
//     computed_parameter.h, storage_policy.h, db.h.
//   - Parameters live statically (owned by the manager, never deleted) and
//     are wired to the pending-write queue at construction time.
//   - No exceptions, C++17 only.
//
// The manager is not thread-safe itself: the UI / C-API layer serialises
// access (single event loop). Individual SubjectT/Parameter values still
// protect themselves with their own mutexes, and PendingWrites has its own
// mutex, so concurrent readers of parameter values remain safe.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <ft8lib/constants.h>

#include "../board.h"
#include "../radio_types.h"
#include "../common/math.h"
#include "computed_parameter.h"
#include "encoder_bind_types.h"
#include "parameter.h"
#include "pending_writes.h"
#include "settings_manager_api.h"

extern "C" {
}

enum ModeGroup {
    MODE_GROUP_SSB = radio_mode_lsb,
    MODE_GROUP_DIGI = radio_mode_lsb_dig,
    MODE_GROUP_CW = radio_mode_cw,
    MODE_GROUP_AM = radio_mode_am,      /* AM and SAM */
    MODE_GROUP_FM = radio_mode_nfm,
    MODE_GROUP_WFM = radio_mode_wfm,    /* its own filter width */
};

class SettingsManager {
  public:
    // Deferred-write buffer (PendingWrites). Declared BEFORE the parameters
    // (C++ member initialisation order) because every Parameter enqueues its
    // changes into this buffer via a WriteSink& bound at construction time.
    // Public because it is the shared sink of the managed parameters;
    // flushing goes through the manager methods (flush_all / flush_storage).
    PendingWrites pending_writes_;

    // Parameter registries (non-owning). MUST be declared before any Parameter
    // member — self-registration in NSDMI constructors pushes into these
    // vectors, so they must already exist (C++ member initialisation order).
    std::vector<ParamBase *> global_params_;
    std::vector<ParamBase *> band_params_;
    std::vector<ParamBase *> mode_params_;

    // The highest TX power p_pwr accepts: BOARD_PWR_MAX, lowered on the X6200's
    // battery (cfg_pwr_max_set). Declared before p_pwr, whose validator reads it.
    // CAT, TCI and the web remote hand their sets to the LVGL thread, which also
    // changes the cap; atomic all the same, as any thread may read a parameter.
    std::atomic<float> pwr_max_{BOARD_PWR_MAX};

    // -- Parameters (public for C-API / UI access; live for the whole program) --
    // All regular GLOBAL/BAND/MODE params are declared with NSDMI here (one
    // line each — the single source of truth). The trailing group argument
    // (&global_params_ / &band_params_ / &mode_params_) makes each Parameter
    // self-register into its registry on construction. VFO params and computed
    // params are deliberately NOT registered (loaded in dependency order by
    // load_band_vfo).

    // --- GLOBAL params (flat `params` table) ---
    // General
    // clang-format off
    Parameter<int32_t> p_volume{"vol", 30, 0, BOARD_VOL_MAX,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float, int32_t, 10> p_pwr{"pwr", 5.0f, [this](float v) { return clip(v, 0.1f, pwr_max_.load()); },
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_squelch{"sql", 0, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_rfgain{"rfgain", 63, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_wefax_align{"wefax_align", 0, -1300, 1300, StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_wefax_tilt{"wefax_tilt", 0, -400, 400, StorageType::GLOBAL, pending_writes_, &global_params_};

    Parameter<int32_t> p_rit{"rit", 0, -1500, 1500,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_xit{"xit", 0, -1500, 1500,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Current band id, persisted in the global `params` table (mirrors the
    // legacy cfg.band_id). Restored at init_load to pick the starting band;
    // updated on every band switch so a restart returns to the last band.
    // Default 7 = 20m SSB, matching the VFOA default frequency of 14.1 MHz.
    Parameter<int32_t> p_band_id{"band_id", 7, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_mic{"mic", radio_mic_auto, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_hmic{"hmic", 20, 0, 50,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_imic{"imic", 30, 0, 50,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    /* The TX monitor off on both radios: at 30 the microphone took the monitor back into the transmission,
     * a loud ringing on PTT in SSB on the X6200 (issue #15), and a feedback loop from the X6100's speaker
     * on PTT and on an app's tones (issue #72) */
    Parameter<int32_t> p_moni{"moni", 0, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_atu_enabled{"atu", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // UI
    Parameter<int32_t> p_auto_level_enabled{"auto_level_enabled", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float, int32_t, 2> p_auto_level_offset{"auto_level_offset", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_knob_info{"knob_info", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Independent VFOs: A and B each keep their own band, a VFO switch brings its band back
    // (vfo_toggle). Off: both VFOs live in the current band, as the radio does it.
    // Turning the knob into another band plan segment sets its mode: CW in the CW segment, the
    // sideband of the band in a phone segment (screen_main.c)
    Parameter<int32_t> p_mode_from_plan{"mode_from_plan", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // The band plan (band_plan.h bp_plan_t): 0 Auto (the IARU region from where the radio is), 1-3 IARU Region 1,
    // 2 or 3, 4-7 the US sub-bands of the Extra, Advanced, General or Technician licence
    Parameter<int32_t> p_band_region{"band_region", 0, 0, 7,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // The IARU region whose default edges the band list's 160m, 80m, 40m SSB and 6m were last given (the defaults
    // in sql/params.sql are Region 1's): a region change moves only the rows still on these (cfg_bands_follow_region).
    // Auto's region when nothing tells (band_plan.c)
    Parameter<int32_t> p_band_edges_region{"band_edges_region", 1, 1, 3,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Zoom and step per mode (Settings > Display): on, each mode group keeps its own waterfall zoom and tuning
    // step, as it always did. Off, one zoom and one step for every mode (zoom_step_scope)
    Parameter<int32_t> p_zoom_step_per_mode{"zoom_step_per_mode", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Birdies (birdie.c): 0 nothing, 1 marked on the waterfall, 2 marked and notched in the passband
    Parameter<int32_t> p_birdie_marks{"birdie_marks", 1, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_indep_vfo{"indep_vfo", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // The WR12, Xiegu's receiver on the X6200's HOST port, is there (Settings > General): 54 MHz-1 GHz to receive.
    // A setting until the radio can tell it is plugged in; read only on the X6200 (band_use)
    Parameter<int32_t> p_wr12{"wr12", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_vfoa_band{"vfoa_band", BAND_UNDEFINED, BAND_UNDEFINED, 1000000,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_vfob_band{"vfob_band", BAND_UNDEFINED, BAND_UNDEFINED, 1000000,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // The spectrum above the waterfall (Settings > Display, live): off by default
    Parameter<int32_t> p_spectrum_show{"spectrum_show", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Its height in px (as upstream bdf4ef63 "Add spectrum height"; live)
    Parameter<int32_t> p_spectrum_height{"spectrum_height", 76, 60, 160,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Decoder text: 0 Large (the stock look), 1 Compact (smaller, more lines); from SP2KMO's upstream c2fbe40c
    Parameter<int32_t> p_digi_text_compact{"digi_text_compact", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_cluster_spots_wf{"cluster_spots_wf", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Walkie Talkie: a portrait screen instead of the main one (live)
    Parameter<int32_t> p_walkie{"walkie", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Walkie Talkie: the main knob steps through the walkie channels instead of the frequency
    Parameter<int32_t> p_walkie_mem{"walkie_mem", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Walkie Talkie: the walkie channel the main knob is on, 0-31 (walkie.c, /mnt/walkie_channels.json)
    Parameter<int32_t> p_walkie_ch{"walkie_ch", 0, 0, 31,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Walkie Talkie: the microphone while its screen is up (radio_mic_sel_t); p_mic stays as saved
    Parameter<int32_t> p_walkie_mic{"walkie_mic", radio_mic_builtin, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // S-meter drawn as a needle on a dial instead of the bars
    // Waterfall colours: 0 the theme's, then classic, gauss, grey, fire, ice, rainbow (theme.c)
    Parameter<int32_t> p_wf_palette{"wf_palette", 0, 0, 6,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Filter passband on the spectrum and waterfall: 0 the theme's, then the presets (theme_filter_color())
    Parameter<int32_t> p_filter_color{"filter_color", 0, 0, 8,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Spectrum trace and fill: 0xRRGGBB, -1 the theme's (the fill: as the trace)
    Parameter<int32_t> p_spectrum_line_color{"spectrum_line_color", -1, -1, 0xffffff,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_spectrum_fill_color{"spectrum_fill_color", -1, -1, 0xffffff,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // The fill fading from its colour at the trace to the background at the bottom (0 = solid)
    Parameter<int32_t> p_spectrum_fill_gradient{"spectrum_fill_gradient", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // S-meter colours: 0 the theme, 1 the scale (green, yellow, red along it), 2 the level (all lit bars)
    Parameter<int32_t> p_smeter_color{"smeter_color", 0, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // With the theme colours (0 above): the S-meter bars / needle scale and the SWR bar in
    // their own 0xRRGGBB, -1 the theme's (after dl2zw's meter and SWR colours, OpenX6100)
    Parameter<int32_t> p_smeter_bar_color{"smeter_bar_color", -1, -1, 0xffffff,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_swr_color{"swr_color", -1, -1, 0xffffff,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // The needle on its lit dial is the default: it suits the RF.Guru theme best (a radio that
    // chose bars keeps them, the saved value wins)
    Parameter<int32_t> p_smeter_needle{"smeter_needle", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Needle S-meter on an amber dial lit from below, as the old analog meters
    Parameter<int32_t> p_smeter_lamp{"smeter_lamp", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_band_plan{"band_plan", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // The broadcast on air (EiBi) in the frequency strip, in place of the band
    Parameter<int32_t> p_broadcast_name{"broadcast_name", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Tuning snap to the cluster spots on the waterfall: detents held on a
    // spot (0 = off) and the most detents per event that still snap (slow)
    Parameter<int32_t> p_spot_snap_hold{"spot_snap_hold", 2, 0, 5,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_spot_snap_slow{"spot_snap_slow", 2, 1, 5,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Main knob acceleration onset: 1 starts only when turned fast, 5 already
    // when turned slowly (3 = upstream's curve)
    Parameter<int32_t> p_freq_accel_onset{"freq_accel_onset", 3, 1, 5,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_cluster_column{"cluster_column", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Cluster lists (column and app): 0 = by frequency around the VFO, 1 = newest first
    Parameter<int32_t> p_cluster_order{"cluster_order", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Cluster lists: 0 = the VFO's band, 1 = all bands
    Parameter<int32_t> p_cluster_all_bands{"cluster_all_bands", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_waterfall_ref{"waterfall_ref", 0, -40, 20,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // S-meter calibration, dB added to the reading. -22: this radio against an IC-7300 on the
    // same antenna. Now only the start value of p_band_smeter_cal on bands never calibrated.
    // RF gain is compensated on its own (meter.c)
    Parameter<int32_t> p_smeter_cal{"smeter_cal", -22, -40, 20,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // The front end, dB taken off the S-meter reading so it stays put when PRE, ATT or the RF gain
    // change: the preamp's gain, the attenuator's depth, and hundredths of a dB for each RF gain
    // step under 100. Defaults measured on one radio; Settings > TX Cal > S-METER > Measure sets
    // them for this one (smeter.c, settings_window.cpp)
    Parameter<int32_t> p_smeter_pre{"smeter_pre", 19, 0, 40,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_smeter_att{"smeter_att", 15, 0, 40,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_smeter_rfg{"smeter_rfg", 60, 0, 200,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Waterfall lines/s: 1 = 5, 2 = 7.5 (frames averaged, less noisy),
    // 3 = 15 (default), 4 = 20, 5 = 30
    Parameter<int32_t> p_waterfall_speed{"waterfall_speed", 3, 1, 5,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Spectrum and waterfall: 0 = VFO centered (the view follows it), 1 = fixed span (the VFO line moves)
    Parameter<int32_t> p_wf_mode{"wf_mode", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // String mapping encoder (cfg_ctrl_t) positions to bind values
    // (ENCODER_BIND_VOL/ENCODER_BIND_MFK/ENCODER_BIND_NONE). GLOBAL: persisted in the flat `params` table under key
    // "encoder_bind", matching the legacy src/cfg key for DB compatibility.
    Parameter<std::string> p_encoder_bind{"encoder_bind", make_default_encoder_bind(), encoder_bind_validate,
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // VOX
    Parameter<int32_t> p_vox_en{"vox_en", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Settings > Audio > Boot sound: "CQ CQ" in CW with the boot animation (bootanim/bootanim.c reads it from the DB)
    Parameter<int32_t> p_boot_sound{"boot_sound", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Settings > Audio > Speaker always off: a remote station's speaker and TX monitor stay silent, whatever the
    // volume (radio.c radio_vol_refresh); the audio to a browser, TCI and headphones goes on
    Parameter<int32_t> p_speaker_off{"speaker_off", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_vox_gain{"vox_gain", 0, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_vox_ag{"vox_ag", 0, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_vox_delay{"vox_delay", 500, 100, 2000,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // FT8
    Parameter<int32_t> p_ft8_show_all{"ft8_show_all", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_ft8_protocol{"ft8_protocol", FTX_PROTOCOL_FT8, 0, 2 /* FT2, ft8/ft2.h */,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_ft8_auto{"ft8_auto", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_ft8_hold_freq{"ft8_hold_freq", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    /* "For you": the callers pinned under the list (off: the list only) */
    Parameter<int32_t> p_ft8_foryou{"ft8_foryou", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_ft8_max_repeats{"ft8_max_repeats", 6, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    /* RX row, right side: 0 km, 1 mi, 2 country, 3 country + km, 4 country + mi
     * (from SP2KMO's upstream 3074180e) */
    Parameter<int32_t> p_ft8_info{"ft8_info", 3, 0, 4,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // WSPR
    Parameter<int32_t> p_wspr_tx_pct{"wspr_tx_pct", 0, 0, 50,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_wspr_tx_freq{"wspr_tx_freq", 1500, 1410, 1590,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_wspr_upload{"wspr_upload", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // WSPR's own TX power, as the dBm the beacon reports (20 = 0.1 W, 23 = 0.2 W ... 37 = 5 W); the radio's power
    // setting is left alone. 23 dBm: 0.2 W is a 0.1 W step of the X6200 and an exact WSPR level, 0.25 W neither
    Parameter<int32_t> p_wspr_dbm{"wspr_dbm", 23, 20, 37,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Decodes (FT8 FT4 FT2 JS8 PSK31 PSK63 Olivia) to PSK Reporter, on by default
    Parameter<int32_t> p_pskreporter{"pskreporter", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // Field Ops: start in Field Ops at power on
    Parameter<int32_t> p_field_boot{"field_boot", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Field Ops: spot again after a QSY (once transmitted there), on by default
    Parameter<int32_t> p_field_respot{"field_respot", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Voice messages (the Field Ops voice CQ too): playback level, dB
    Parameter<int32_t> p_voice_msg_gain{"voice_msg_gain", 0, -20, 10,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // SWR scan
    Parameter<int32_t> p_swrscan_linear{"swrscan_linear", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_swrscan_span{"swrscan_span", 200'000, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // CW
    Parameter<int32_t> p_key_tone{"key_tone", 700, 400, 1200,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_key_speed{"key_speed", 15, 5, 50,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_key_mode{"key_mode", radio_key_manual, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_iambic_mode{"iambic_mode", radio_iambic_a, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_key_vol{"key_vol", 10, 0, 32,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_key_train{"key_train", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // SWR protection: the most SWR a transmission keeps, in tenths (10 = off)
    Parameter<int32_t> p_swr_protect{"swr_protect", 20, 10, 50,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_qsk_time{"qsk_time", 100, 0, 1000,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float, int32_t, 10> p_key_ratio{"key_ratio", 3.0f, 2.5f, 4.5f,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t>            p_cw_peak_on{"cw_peak_on", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t>            p_cw_peak_q{"cw_peak_q", 1, 1, 16,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // USB paddle (paddle_keyer.c): per source the left code | the right one << 16
    Parameter<int32_t> p_paddle_kbd{"paddle_kbd", 29 | 97 << 16, 0, INT32_MAX,        // Left Ctrl, Right Ctrl
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_paddle_btn{"paddle_btn", 0x110 | 0x111 << 16, 0, INT32_MAX,  // BTN_LEFT, BTN_RIGHT
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_paddle_midi{"paddle_midi", 1 | 2 << 16, 0, INT32_MAX,        // Vail adapter notes
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_paddle_ser{"paddle_ser", 0x020 | 0x100 << 16, 0, INT32_MAX,  // TIOCM_CTS, TIOCM_DSR
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_paddle_swap{"paddle_swap", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Off: a USB GPS on ttyUSB could hold CTS/DSR and key the radio
    Parameter<int32_t> p_paddle_serial{"paddle_serial", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // CW trainer (Koch level = number of characters in use, 2 = K M)
    Parameter<int32_t> p_cwt_level{"cwt_level", 2, 2, 40,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_cwt_group{"cwt_group", 1, 1, 5,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // CW decoder
    Parameter<int32_t> p_cw_decoder{"cw_decoder", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_cw_tune{"cw_tune", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Detection floor of the decoder (SNR in its ~60 Hz detection bandwidth).
    // New key: the old "cw_decoder_snr_2" value had another meaning.
    Parameter<float, int32_t, 10> p_cw_decoder_snr{"cw_decoder_snr_3", 8.0f, 5.0f, 30.0f,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Decoded words split and coloured: calls, abbreviations, RST (cw_parse.c)
    Parameter<int32_t> p_cw_parse{"cw_parse", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // CW keyboard and the main screen CW line: each letter keyed as typed
    Parameter<int32_t> p_cw_live{"cw_live", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // AGC
    Parameter<int32_t> p_agc_hang{"agc_hang", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_agc_knee{"agc_knee", -60, -100, 0,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_agc_slope{"agc_slope", 6, 0, 10,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // DSP
    Parameter<int32_t> p_dnf{"dnf", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_dnf_center{"dnf_center", 1000, 100, 3000,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_dnf_width{"dnf_width", 50, 10, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_dnf_auto{"dnf_auto", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nb{"nb", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nb_level{"nb_level", 10, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nb_width{"nb_width", 10, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nr{"nr", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nr_level{"nr_level", 0, 0, 60,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // DSP custom
    Parameter<float, int32_t, 5> p_output_gain{"output_gain", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
#ifdef BOARD_X6200
    /* Off, as gdyuldin's X6200 GUI starts (and as the X6200 had it in effect while the bit went in upside down) */
    Parameter<int32_t>           p_comp{"comp", 1, 1, 8,
#else
    Parameter<int32_t>           p_comp{"comp", 4, 1, 8,
#endif
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float, int32_t, 2> p_comp_threshold_offset{"comp_threshold_offset", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<float, int32_t, 2> p_comp_makeup_offset{"comp_makeup_offset", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t>            p_fm_emphasis{"fm_emphasis", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t>            p_tx_filter_low{"tx_filter_low", 160, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t>            p_tx_filter_high{"tx_filter_high", 3000, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t>            p_cessb_on{"cessb_on", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float, int32_t, 10> p_cessb_power_up{"cessb_power_up", 3.7f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // --- BAND params (`band_params` table) ---
    Parameter<int32_t> p_band_current_vfo{"vfo", (int32_t)RADIO_VFO_A, 0, 1,
        StorageType::BAND, pending_writes_, &band_params_};
    Parameter<int32_t> p_band_if_shift{"if_shift", 0, -40'000, 40'000,
        StorageType::BAND, pending_writes_, &band_params_};
    Parameter<int32_t> p_band_grid_min{"grid_min", -121, {},
        StorageType::BAND, pending_writes_, {}, &band_params_};
    Parameter<int32_t> p_band_grid_max{"grid_max", -73, {},
        StorageType::BAND, pending_writes_, {}, &band_params_};
    // Split, this band's own: a band that never saved one starts without, not with the last band's (a split a TX
    // shift turned on stayed on in every band after)
    Parameter<int32_t> p_band_split{"split", false, [](int32_t v) { return clip(v, 0, 1); },
        StorageType::BAND, pending_writes_, [this] { p_band_split.set_quiet(0); }, &band_params_};
    // TX shift of this band, Hz, 0 off (|shift| up to 10 MHz): with split on, the other VFO is kept at the active one
    // + the shift (tx_shift_follow), so the radio transmits shifted: an FM repeater's input, a QO-100 station's IF.
    // A band that never saved one has none, not the last band's
    Parameter<int32_t> p_band_tx_shift{"tx_shift", 0, [](int32_t v) { return clip(v, -TX_SHIFT_MAX, TX_SHIFT_MAX); },
        StorageType::BAND, pending_writes_, [this] { p_band_tx_shift.set_quiet(0); }, &band_params_};
    Parameter<int32_t> p_band_tx_i_offset{"tx_i_offset", 0, {},
        StorageType::BAND, pending_writes_, {}, &band_params_};
    Parameter<int32_t> p_band_tx_q_offset{"tx_q_offset", 0, {},
        StorageType::BAND, pending_writes_, {}, &band_params_};
    // dB offset of the front panel audio chain (float, scaled x10 to an int
    // in the DB: 0.2 dB <-> 2).
    Parameter<float, int32_t, 10> p_band_dac_offset{"dac_offset", 0.0f, {},
        StorageType::BAND, pending_writes_, {}, &band_params_};
    // S-meter calibration of this band, dB (Settings, S-meter cal): the baseband reads a
    // different level on each band. A band never set takes the old global smeter_cal
    Parameter<int32_t> p_band_smeter_cal{"smeter_cal", -22, [](int32_t v) { return clip(v, -40, 40); },
        StorageType::BAND, pending_writes_, [this] { p_band_smeter_cal.set_quiet(p_smeter_cal.get()); },
        &band_params_};
    // BAND VFO params (context_id = bands_id). These are NOT in the band
    // registry: load_band_vfo() loads them in explicit dependency order so the
    // DB restore/clamp rules can reference already-loaded siblings. Up to 1 GHz: the WR12's top
    Parameter<int32_t> p_band_vfoa_freq{"vfoa_freq", 14'100'000, 100'000, 1'000'000'000,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_freq{"vfob_freq", 14'150'000, 100'000, 1'000'000'000,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfoa_mode{"vfoa_mode", radio_mode_usb, (int32_t)radio_mode_lsb, (int32_t)radio_mode_last,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_mode{"vfob_mode", radio_mode_usb, (int32_t)radio_mode_lsb, (int32_t)radio_mode_last,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfoa_att{"vfoa_att", radio_att_off, 0, (int32_t)radio_att_on,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfoa_pre{"vfoa_pre", radio_pre_off, 0, (int32_t)radio_pre_on,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfoa_agc{"vfoa_agc", radio_agc_auto, 0, (int32_t)radio_agc_auto,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_att{"vfob_att", radio_att_off, 0, (int32_t)radio_att_on,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_pre{"vfob_pre", radio_pre_off, 0, (int32_t)radio_pre_on,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_agc{"vfob_agc", radio_agc_auto, 0, (int32_t)radio_agc_auto,
        StorageType::BAND, pending_writes_};

    // --- Transverter params (OTHER storage, fixed HW conversion) ---
    // context_id is the fixed transverter number (0 or 1) — never switched. NOT registered
    // in any load-all registry (group = nullptr): they are loaded individually
    // in init_load() with explicit load(0)/load(1). Values are frequencies in Hz.
    // A shift of 0 is a transverter that is off. The X6200's are off by default: its 2 m and 70 cm are the WR12's,
    // to receive, and the always-on transverters tuned "145.500" to 29.500 MHz and transmitted there, on 10 m.
    // Migration _11 keeps the shift of one set up before. The X6100 keeps upstream's two, on by default
    // Transverter 0 (2m: 144-150 MHz, IF at 28 MHz)
    Parameter<int32_t> p_transverter_0_from{"from", 144'000'000, 70'000'000, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 0};
    Parameter<int32_t> p_transverter_0_to{"to", 150'000'000, 70'000'000, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 0};
    Parameter<int32_t> p_transverter_0_shift{"shift", HAS_VHF_RX ? 0 : 116'000'000, 0, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 0};
    // Transverter 1 (70cm: 432-438 MHz, IF at 28 MHz)
    Parameter<int32_t> p_transverter_1_from{"from", 432'000'000, 70'000'000, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 1};
    Parameter<int32_t> p_transverter_1_to{"to", 438'000'000, 70'000'000, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 1};
    Parameter<int32_t> p_transverter_1_shift{"shift", HAS_VHF_RX ? 0 : 404'000'000, 0, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 1};

    // --- MODE params (`mode_params` table) ---
    // Parameter<int32_t> p_mode_squelch{
    //     "squelch",    0, StorageType::MODE, pending_writes_, [](int32_t v) { return clip(v, 0, 100); }, {},
    //     &mode_params_};
    // The step and the zoom are the current mode group's, or every mode's (context MODE_ALL) while
    // p_zoom_step_per_mode is off: their readers and writers never see the difference
    Parameter<int32_t> p_mode_freq_step{"freq_step", 500, [](int32_t v) { return clip(v, 1, 10000); },
        StorageType::MODE, pending_writes_, [this]() { on_mode_freq_step_not_found(); }, &mode_params_};
    Parameter<int32_t> p_mode_zoom{"spectrum_factor", 1, [](int32_t v) { return clip(v, 1, 8); },
        StorageType::MODE, pending_writes_, [this]() { on_mode_zoom_not_found(); }, &mode_params_};

  private:
    // MODE-scoped filter band edges. PRIVATE: never read/written directly
    // outside this class; external C++ reads the effective values via the
    // public computed params cp_cur_filter_low/high/bw and mutates them
    // through their set() (reverse path). Still self-register into
    // mode_params_ so load_mode_all/switch_mode load them like any mode param.
    // Cross-validators reference the sibling member; they only run on set /
    // set_quiet (never during construction), by which time both exist.
    Parameter<int32_t> p_mode_filter_low{"filter_low", 50, [this](int32_t v) { return filter_low_validate(v); },
        StorageType::MODE, pending_writes_, [this]() { on_mode_filter_low_not_found(); }, &mode_params_};
    Parameter<int32_t> p_mode_filter_high{"filter_high", 2900, [this](int32_t v) { return filter_high_validate(v); },
        StorageType::MODE, pending_writes_, [this]() { on_mode_filter_high_not_found(); }, &mode_params_};
    // clang-format on

  public:
    // Computed parameter: front-panel ("FG") frequency = the frequency of the
    // active VFO of the current band. Constructed with compute/reverse closures
    // in the SettingsManager ctor (no default ctor exists).
    ComputedParameter<int32_t> cp_fg_freq;

    // Computed parameter: the mode of the active VFO of the current band.
    // Analogous to cp_fg_freq: p_band_current_vfo selects between the VFO A/B
    // mode params. Constructed with compute/reverse closures in the ctor.
    ComputedParameter<int32_t> cp_cur_mode;

    // Computed parameters for the current VFO's att/pre/agc and the background
    // VFO frequency. Att/pre/agc track the ACTIVE VFO's value; bg_freq returns
    // the INACTIVE VFO's frequency. Constructed with closures in the ctor,
    // sources bound in init_load().
    ComputedParameter<int32_t> cp_cur_att;
    ComputedParameter<int32_t> cp_cur_pre;
    ComputedParameter<int32_t> cp_cur_agc;
    ComputedParameter<int32_t> cp_bg_freq;

    // Computed mode LO offset: CW → -key_tone, CWR → +key_tone, else 0.
    // Driven by cp_cur_mode and p_key_tone.
    ComputedParameter<int32_t> cp_mode_lo_offset;

    // Computed current filter params for the active MODE category. The compute
    // fns map the MODE-scoped filter_low/filter_high (and key_tone for CW)
    // through the mode category (SSB/AM/FM/CW); the reverse fns write back into
    // them. Modelled on cp_fg_freq/cp_cur_mode: constructed with closures in the
    // ctor (no default ctor), sources bound in init_load().
    ComputedParameter<int32_t> cp_cur_filter_low;
    ComputedParameter<int32_t> cp_cur_filter_high;
    ComputedParameter<int32_t> cp_cur_filter_bw;

    // -- Construction / lifetime --
    explicit SettingsManager();
    ~SettingsManager();

    SettingsManager(const SettingsManager &)            = delete;
    SettingsManager &operator=(const SettingsManager &) = delete;
    SettingsManager(SettingsManager &&)                 = delete;
    SettingsManager &operator=(SettingsManager &&)      = delete;

    // -- Initialisation --
    // Loads global params (including the persisted band_id), then band params
    // for the restored band and mode params for the mode derived from the
    // active VFO (cp_cur_mode). on_db_error (optional) is invoked on DB
    // failures; parameters keep their current values on NOT_FOUND.
    void init_load(void (*on_db_error)(const char *msg) = nullptr);

    // -- PendingWrites --
    void flush_all();
    void flush_storage(StorageType type, int context_id);

    // Background deferred-save thread: wakes every 3 seconds (or on notify)
    // and calls flush_all().
    //
    // The flush thread must satisfy PendingWrites invariants I1/I2 (see
    // pending_writes.h): it only reads the queue's own snapshots + the
    // stateless policies and never touches SettingsManager member state
    // (band_id_, mode_id_, the parameters), so it cannot race manager context
    // or param mutation.
    void start_flush_thread();
    void stop_flush_thread();

    // -- Accessors --
    int current_band_id() const { return band_id_; }
    int current_mode_group_id() const { return mode_group_id_; }

    // The mode_params rows of the zoom and the step every mode shares (p_zoom_step_per_mode off): no mode group's,
    // and not -1, which load() and flush_storage() take for "the bound one" and "any"
    static constexpr int MODE_ALL = 100;

    // Hardware-usable frequency range limits (in Hz). HF is the native
    // 0.1-55 MHz band; anything above must be covered by a transverter.
    static constexpr int32_t HF_MIN_FREQ = 100'000;     /* upstream f6c97fa */
    static constexpr int32_t HF_MAX_FREQ = 55'000'000;
    // The X6200 also receives FM broadcast (88-108 MHz, wide FM) and the airband (108-136 MHz, AM), as its
    // specification and the OEM firmware have it; receive only (issue #9)
    static constexpr int32_t VHF_RX_MIN_FREQ = 87'500'000;
    static constexpr int32_t VHF_RX_MAX_FREQ = 137'000'000;
#ifdef BOARD_X6200
    static constexpr bool HAS_VHF_RX = true;
#else
    static constexpr bool HAS_VHF_RX = false;
#endif
    // The WR12 on the X6200's HOST port (p_wr12): 54 MHz-1 GHz, receive only
    static constexpr int32_t WR12_MIN_FREQ = 54'000'000;
    static constexpr int32_t WR12_MAX_FREQ = 1'000'000'000;

    // The largest TX shift, Hz (p_band_tx_shift)
    static constexpr int32_t TX_SHIFT_MAX = 10'000'000;

    // -- VFO operations --
    // Copies VFO-related band parameters (freq, mode, agc, att, pre) from the
    // active VFO to the inactive VFO, based on the current p_band_current_vfo.
    void cfg_band_vfo_copy();

    // The other VFO. With independent VFOs on, the band this VFO is on is kept for it and the
    // other VFO's band is loaded (its frequency and mode come with the band's record).
    void vfo_toggle();
    void band_follow(int32_t freq);    // the band the active VFO's frequency lies in, implicitly

    // The TX shift of the current band, Hz (Settings, the web Bands page): split comes on with a shift, off with 0
    void tx_shift_set(int32_t hz);

    // Transverter shift for a frequency: the shift of the transverter whose
    // [from, to] range contains `freq`, or 0 when no transverter covers it.
    // A transverter with a shift of 0 is off and covers nothing.
    int32_t transverter_shift_for(int32_t freq) const {
        if (p_transverter_0_shift.get() && freq >= p_transverter_0_from.get() && freq <= p_transverter_0_to.get())
            return p_transverter_0_shift.get();
        if (p_transverter_1_shift.get() && freq >= p_transverter_1_from.get() && freq <= p_transverter_1_to.get())
            return p_transverter_1_shift.get();
        return 0;
    }

    // What the radio can do somewhere in [lo, hi]: TX where HF 0.1-55 MHz or a transverter with a shift covers part
    // of it, else RX where the X6200 receives (its FM broadcast and airband; 54 MHz-1 GHz with the WR12 on), else
    // nothing. A transverter wins where they overlap. Bands-table ranges are informational only.
    band_use_t band_use(int32_t lo, int32_t hi) const {
        auto in = [lo, hi](int32_t a, int32_t b) { return lo <= b && hi >= a; };

        if (in(HF_MIN_FREQ, HF_MAX_FREQ)
            || (p_transverter_0_shift.get() && in(p_transverter_0_from.get(), p_transverter_0_to.get()))
            || (p_transverter_1_shift.get() && in(p_transverter_1_from.get(), p_transverter_1_to.get())))
            return BAND_USE_TX;
        if (HAS_VHF_RX && (in(VHF_RX_MIN_FREQ, VHF_RX_MAX_FREQ) || (p_wr12.get() && in(WR12_MIN_FREQ, WR12_MAX_FREQ))))
            return BAND_USE_RX;
        return BAND_USE_NONE;
    }

    // True when `freq` is usable by the hardware, to receive at least
    bool is_valid_hw_freq(int32_t freq) const { return band_use(freq, freq) != BAND_USE_NONE; }
    // The radio receives `freq` but must not transmit there (the X6200's FM broadcast and airband, the WR12)
    bool is_rx_only_freq(int32_t freq) const { return band_use(freq, freq) == BAND_USE_RX; }
    // The radio transmits on `freq`
    bool is_tx_freq(int32_t freq) const { return band_use(freq, freq) == BAND_USE_TX; }

    // If `freq` is hardware-usable, return it unchanged. Otherwise clamp to the
    // nearest boundary of any hardware-usable frequency range (HF 0.1-55 MHz,
    // the transverters that are on, the X6200's receive ranges).
    int32_t clamp_to_valid_hw_freq(int32_t freq) const;

    // The mode a frequency opens in when its band has none saved: its sideband, LSB under 10 MHz and USB above
    // (and on 60 m, where USB is used); wide FM and AM on the X6200's FM broadcast and airband, NFM where only the
    // WR12 receives
    int32_t default_mode(int32_t freq) const;

  private:
    // active VFO frequency of the current band (compute fn for cp_fg_freq).
    int32_t fg_freq_get();

    // reverse fn for cp_fg_freq: writes freq into the active VFO param.
    void fg_freq_set(int32_t freq);

    // compute/reverse fns for cp_bg_freq: the background (inactive) VFO's
    // frequency, i.e. the complement of the active VFO.
    int32_t bg_freq_get();
    void    bg_freq_set(int32_t freq);

    // active VFO mode of the current band (compute fn for cp_cur_mode).
    int32_t cur_mode_get();

    // reverse fn for cp_cur_mode: writes mode into the active VFO's mode param.
    void cur_mode_set(int32_t mode);

    // compute/reverse fns for cp_cur_att / cp_cur_pre / cp_cur_agc: read/write
    // the active VFO's att/pre/agc param based on p_band_current_vfo.
    int32_t cur_att_get();
    void    cur_att_set(int32_t att);
    int32_t cur_pre_get();
    void    cur_pre_set(int32_t pre);
    int32_t cur_agc_get();
    void    cur_agc_set(int32_t agc);

    // Trampoline for the cp_cur_mode observer: calls switch_mode() with the new
    // mode whenever cp_cur_mode's value changes.
    static void switch_mode_observer_cb(Subject *subj, void *user_data);

    // Trampoline for the p_band_vfoa_freq / p_band_vfob_freq observers: when a
    // VFO frequency changes into a different band, triggers an implicit band
    // switch (the frequency that caused the switch is preserved).
    static void vfo_freq_change_cb(Subject *subj, void *user_data);

    // Trampoline for the p_band_id observer: an explicit band switch is triggered
    // by setting p_band_id; the observer runs switch_band with implicit=false.
    static void switch_band_observer_cb(Subject *subj, void *user_data);

    // The TX shift kept: while the band has one and split is on, the other VFO is the active one + the shift (on
    // VFO A: B = A + shift; on VFO B: A = B - shift, the reverse), in the active one's mode. Run on every change of
    // either VFO's frequency or mode, the active VFO, split and the shift, so any path that tunes (knob, memory,
    // CAT, TCI, the apps, a band switch) brings the TX frequency along; a write to the other VFO is put back
    void tx_shift_follow();
    static void tx_shift_cb(Subject *subj, void *user_data);

    // Mode group functions
    static ModeGroup mode_group(radio_mode_t mode);
    static int32_t mode_group_filter_low_default(ModeGroup group);
    static int32_t mode_group_filter_high_default(ModeGroup group);
    static int32_t mode_group_freq_step_default(ModeGroup group);
    static int32_t mode_group_zoom_default(ModeGroup group);

    void on_mode_filter_low_not_found();
    void on_mode_filter_high_not_found();
    void on_mode_freq_step_not_found();
    void on_mode_zoom_not_found();

    // Shared band-switch core: flush, rebind context, switch-time loads and
    // recomputes. `implicit`: the active VFO's frequency is kept, and the other VFO
    // too unless the new band has split on (load_band_vfo).
    void switch_band(int new_band_id, bool implicit);

    // -- Context switching --
    // Switches mode context: saves pending mode writes and loads mode params
    // for new_group_id.
    void switch_mode(ModeGroup new_group_id);

    // Apply the current band/mode context to every parameter in the group. A mode param bound to MODE_ALL keeps it.
    void set_band_context(int band_id);
    void set_mode_context(int mode_id);

    // Full group loads (used by init_load). load_mode_all, also a mode switch's, leaves the params bound to MODE_ALL.
    void load_band_all(int band_id);
    void load_mode_all();

    // The zoom and the step bound as p_zoom_step_per_mode has it: to the current mode group, or to MODE_ALL. At
    // init the values saved for every mode are read; the option turned off, the current mode's become every mode's.
    // Back per mode, each mode's own values return: the shared ones were never written into their rows
    void zoom_step_scope(bool at_init);
    static void zoom_step_scope_cb(Subject *subj, void *user_data);

    // Switch-time band loads: never loads current_vfo; when `implicit` is true
    // the VFOs as load_band_vfo keeps them.
    void load_band_switch(int new_band_id, bool implicit);

    // Loads the four VFO params (vfoa_freq, vfoa_mode, vfob_freq, vfob_mode) in
    // explicit dependency order, applying the DB restore/clamp rules:
    //   - vfoa_freq: NOT_FOUND -> BandInfo.start_freq (or 12000000 if the band
    //     is undefined/missing); a loaded value outside [start,stop] is clamped
    //     to start_freq.
    //   - vfoa_mode: NOT_FOUND/error -> resolve_default_mode(vfoa_freq).
    //   - vfob_freq: NOT_FOUND -> current vfoa_freq; else clamped as vfoa_freq.
    //   - vfob_mode: NOT_FOUND/error -> current vfoa_mode.
    // Restored/clamped values are applied with set_quiet (no deferred write).
    // When `implicit` the active VFO's frequency is kept and the other VFO keeps
    // all it had, unless the new band has split on (then it is loaded as above).
    void load_band_vfo(int band_id, bool implicit);

    // Default mode for a band with none saved: default_mode(freq), AM in the broadcast bands
    int32_t resolve_default_mode(int32_t freq, int32_t band_id) const;

    // Default 50-char encoder-bind string (all NONE except the documented
    // VOL/MFK positions). Static: callable from the ctor init list.
    static std::string make_default_encoder_bind();

    // Validate/pad the encoder-bind string to CTRL_FAST_ACCESS_LAST chars
    // (filling any shorter input with ENCODER_BIND_NONE). Pure function.
    static std::string encoder_bind_validate(const std::string &val);

    // Filter category of a mode (SSB/AM/FM/CW; unknown/default -> SSB).
    enum class FilterMode {
        SSB,
        AM,
        FM,
        CW
    };
    FilterMode filter_mode(int32_t mode) const;

    // Validate filters (cross-validate against the sibling filter edge; read
    // member params via `this`).
    int32_t filter_low_validate(int32_t v);
    int32_t filter_high_max();
    int32_t filter_high_validate(int32_t v);

    // compute fns for cp_cur_filter_{low,high,bw}.
    int32_t cur_filter_low_compute();
    int32_t cur_filter_high_compute();
    int32_t cur_filter_bw_compute();

    int32_t lo_offset_compute();

    // reverse fns for cp_cur_filter_{low,high,bw}: map back into
    // p_mode_filter_low/high per the current mode category.
    void cur_filter_low_reverse(int32_t v);
    void cur_filter_high_reverse(int32_t v);
    void cur_filter_bw_reverse(int32_t v);

    // -- Member data --
    int band_id_ = 0;
    int mode_group_id_ = 0;

    // Re-entrancy guard for switch_band: prevents the p_band_id observer (which
    // fires from inside switch_band's p_band_id.set) from running a nested
    // switch_band with a different `implicit` flag.
    bool band_switch_active_ = false;

    // Holds the cp_cur_mode observer that triggers switch_mode() on mode change.
    // RAII: unsubscribes on destruction.
    Subscription switch_mode_obs_;

    // The p_zoom_step_per_mode observer: rebinds the zoom and the step (zoom_step_scope)
    Subscription zoom_step_obs_;

    // Holds the p_band_id observer that triggers an explicit band switch when
    // p_band_id is set. RAII: unsubscribe on destruction.
    Subscription band_id_obs_;

    // Holds the VFO frequency observers that trigger an implicit band switch
    // when the active VFO is tuned into a different band. RAII: unsubscribe on
    // destruction.
    Subscription vfoa_freq_obs_;
    Subscription vfob_freq_obs_;

    // The TX shift's observers (both VFO frequencies and modes, the active VFO, split, the shift) and its re-entrancy
    // guard: tx_shift_follow writes a VFO it watches
    Subscription tx_shift_obs_[7];
    bool         tx_shift_busy_ = false;

    // Background flush thread control.
    bool                    flush_thread_running_ = false;
    std::thread             flush_thread_;
    std::condition_variable flush_cv_;
    std::mutex              flush_mutex_;
};

// Global SettingsManager instance (defined in cfg_api.cpp). Accessed directly
// by C++ code (e.g. cat.cpp) without going through the C API layer.
extern SettingsManager cfg_sm;
