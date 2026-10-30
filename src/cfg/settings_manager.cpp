#include "settings_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>

extern "C" {
}


// SettingsManager implementation.
//
// Band/mode switching semantics (aligned with old src/cfg/band.c):
//   - Explicit band switch: setting p_band_id triggers an observer that calls
//     switch_band(implicit=false). Save the pending writes of the current band,
//     then load all band params of the new band EXCEPT current_vfo (the active
//     VFO reference stays the same VFO). With independent VFOs the other VFO
//     keeps all it had, as on an implicit switch, unless the new band has split on.
//   - Implicit switch: when a VFO frequency is tuned into a different band
//     (via cp_fg_freq or the active p_band_vfo*_freq), an observer triggers a
//     band switch. The active VFO's frequency that caused the switch is kept,
//     but its mode/att/pre/agc are loaded from the new band. The other VFO
//     keeps all it had, unless the new band has split on.
//   - Switch mode: the user changed the mode. Save pending mode writes and
//     load the mode params for the new mode; not the zoom and the step while
//     every mode shares them (p_zoom_step_per_mode off, zoom_step_scope).
//
// All loads use Parameter::load(), which reads the context_id bound via
// set_context_id() — no context threading through every call.

SettingsManager::SettingsManager()
    : // Computed front-panel frequency: the frequency of the active VFO of the
      // current band. The compute function reads the band VFO params; the
      // reverse function writes back into the active VFO's frequency param.
      // Sources (VFO frequency params + current_vfo) are bound once in
      // init_load() via ComputedParameter::bind().
      // The background VFO frequency is the complement of the active one.
      cp_fg_freq([this]() { return fg_freq_get(); }, [this](int32_t freq) { fg_freq_set(freq); }),
      cp_bg_freq([this]() { return bg_freq_get(); }, [this](int32_t freq) { bg_freq_set(freq); }),

      // Computed current mode: the mode of the active VFO of the current band.
      // Analogous to cp_fg_freq. Sources (VFO mode params + current_vfo) are
      // bound once in init_load(); an observer on this subject triggers
      // switch_mode() whenever the current mode changes.
      cp_cur_mode([this]() { return cur_mode_get(); }, [this](int32_t mode) { cur_mode_set(mode); }),

      // Computed current VFO's att/pre/agc. The compute fns read the active
      // VFO's value; the reverse fns write back into it.
      cp_cur_att([this]() { return cur_att_get(); }, [this](int32_t att) { cur_att_set(att); }),
      cp_cur_pre([this]() { return cur_pre_get(); }, [this](int32_t pre) { cur_pre_set(pre); }),
      cp_cur_agc([this]() { return cur_agc_get(); }, [this](int32_t agc) { cur_agc_set(agc); }),

      // Mode LO offset: CW→-key_tone, CWR→+key_tone, else 0. One-way (no reverse).
      cp_mode_lo_offset([this]() { return lo_offset_compute(); }),

      // Computed current filter params. The compute fns derive the effective
      // filter edges/bw from the MODE-scoped filter params (and key_tone for
      // CW) according to the active mode's category; the reverse fns write
      // back into filter_low/filter_high. Sources are bound in init_load().
      cp_cur_filter_low([this]() { return cur_filter_low_compute(); },
                        [this](int32_t v) { cur_filter_low_reverse(v); }),
      cp_cur_filter_high([this]() { return cur_filter_high_compute(); },
                         [this](int32_t v) { cur_filter_high_reverse(v); }),
      cp_cur_filter_bw([this]() { return cur_filter_bw_compute(); }, [this](int32_t v) { cur_filter_bw_reverse(v); })

{
    // All Parameter members self-register via NSDMI in the header — no
    // registration calls needed here. The constructor is empty.
}

SettingsManager::~SettingsManager() {
    stop_flush_thread();
}

void SettingsManager::init_load(void (*on_db_error)(const char *msg)) {
    // Global params (flat `params` table; context_id is ignored). The unified
    // ParamBase registry now includes p_pwr (float), p_encoder_bind (text) and
    // p_band_id (the persisted current band), so the non-int32 special cases
    // no longer need explicit load calls.
    for (ParamBase *p : global_params_) {
        int rc = p->load(0);
        if (rc != SUCCESS && rc != NOT_FOUND && on_db_error) {
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Failed to load %s", p->db_name());
            on_db_error(msg);
        }
    }

    // Transverter params are loaded individually (fixed context_id = the
    // transverter number). NOT_FOUND keeps the default value. Before the band:
    // the clamps of load_band_vfo ask band_use, which reads them.
    p_transverter_0_from.load(0);
    p_transverter_0_to.load(0);
    p_transverter_0_shift.load(0);
    p_transverter_1_from.load(1);
    p_transverter_1_to.load(1);
    p_transverter_1_shift.load(1);

    // The starting band comes from the persisted global parameter.
    band_id_ = p_band_id.get();

    set_band_context(band_id_);

    load_band_all(band_id_);

    // Subscribe the VFO frequency observers that trigger an implicit band
    // switch when a frequency is tuned into a different band. Subscribed after
    // the initial load so the initial restore does not trigger a switch.
    vfoa_freq_obs_ = Subscription(p_band_vfoa_freq.subscribe(vfo_freq_change_cb, this));
    vfob_freq_obs_ = Subscription(p_band_vfob_freq.subscribe(vfo_freq_change_cb, this));

    // The TX shift follows the VFOs: after the band follow above, so a tune into another band has loaded that
    // band's shift and split when it runs
    Subject *tx_shift_subj[] = {&p_band_vfoa_freq, &p_band_vfob_freq, &p_band_vfoa_mode, &p_band_vfob_mode,
                                &p_band_current_vfo, &p_band_split, &p_band_tx_shift};
    for (size_t i = 0; i < std::size(tx_shift_obs_); i++)
        tx_shift_obs_[i] = Subscription(tx_shift_subj[i]->subscribe(tx_shift_cb, this));

    // Subscribe the p_band_id observer that triggers an explicit band switch
    // when p_band_id is set. Subscribed after the initial load so the restore
    // of the persisted band does not trigger a switch.
    band_id_obs_ = Subscription(p_band_id.subscribe(switch_band_observer_cb, this));

    // Bind the computed fg_freq to its sources (VFO frequency params +
    // current_vfo) so it recomputes automatically when they change.
    cp_fg_freq.bind(p_band_vfoa_freq);
    cp_fg_freq.bind(p_band_vfob_freq);
    cp_fg_freq.bind(p_band_current_vfo);

    // Recompute fg_freq from the freshly loaded band params.
    cp_fg_freq.recompute();

    // Bind the computed cur_mode to its sources (VFO mode params + current_vfo)
    // and recompute it BEFORE deriving the mode id, so the initial recompute
    // does not fire switch_mode().
    cp_cur_mode.bind(p_band_vfoa_mode);
    cp_cur_mode.bind(p_band_vfob_mode);
    cp_cur_mode.bind(p_band_current_vfo);
    cp_cur_mode.recompute();

    // The starting mode is the active VFO's mode of the restored band.
    mode_group_id_ = mode_group((radio_mode_t)cp_cur_mode.get());

    set_mode_context(mode_group_id_);

    load_mode_all();

    // Zoom and step of every mode (the option off): bound to their own rows after the mode's are loaded, so a
    // row never written takes the current mode's values
    zoom_step_scope(true);

    // Subscribe an observer that calls switch_mode() whenever the current mode
    // changes (e.g. from cp_cur_mode.set() or a VFO switch).
    switch_mode_obs_ = Subscription(cp_cur_mode.subscribe(switch_mode_observer_cb, this));
    zoom_step_obs_   = Subscription(p_zoom_step_per_mode.subscribe(zoom_step_scope_cb, this));

    // Bind the computed current VFO att/pre/agc and the background VFO
    // frequency to their sources and recompute from the freshly loaded params.
    cp_cur_att.bind(p_band_vfoa_att);
    cp_cur_att.bind(p_band_vfob_att);
    cp_cur_att.bind(p_band_current_vfo);
    cp_cur_att.recompute();

    cp_cur_pre.bind(p_band_vfoa_pre);
    cp_cur_pre.bind(p_band_vfob_pre);
    cp_cur_pre.bind(p_band_current_vfo);
    cp_cur_pre.recompute();

    cp_cur_agc.bind(p_band_vfoa_agc);
    cp_cur_agc.bind(p_band_vfob_agc);
    cp_cur_agc.bind(p_band_current_vfo);
    cp_cur_agc.recompute();

    cp_bg_freq.bind(p_band_vfoa_freq);
    cp_bg_freq.bind(p_band_vfob_freq);
    cp_bg_freq.bind(p_band_current_vfo);
    cp_bg_freq.recompute();

    cp_mode_lo_offset.bind(cp_cur_mode);
    cp_mode_lo_offset.bind(p_key_tone);
    cp_mode_lo_offset.recompute();

    // Bind the computed filter params to their sources (filter_low/high +
    // key_tone) so reverse writes to them propagate to the sibling cur_* and
    // direct p_mode_filter_*.set() keeps cur_* in sync. Recompute after the
    // mode params have been loaded.
    cp_cur_filter_low.bind(p_mode_filter_low);
    cp_cur_filter_low.bind(p_mode_filter_high);
    cp_cur_filter_low.bind(p_key_tone);
    cp_cur_filter_high.bind(p_mode_filter_low);
    cp_cur_filter_high.bind(p_mode_filter_high);
    cp_cur_filter_high.bind(p_key_tone);
    cp_cur_filter_bw.bind(cp_cur_filter_low);
    cp_cur_filter_bw.bind(cp_cur_filter_high);
    cp_cur_filter_low.recompute();
    cp_cur_filter_high.recompute();
    cp_cur_filter_bw.recompute();

    // A band left with a shift: its TX frequency as the shift has it, whatever was saved
    tx_shift_follow();
}

void SettingsManager::on_mode_filter_low_not_found() {
    p_mode_filter_low.set_quiet(mode_group_filter_low_default((ModeGroup)p_mode_filter_low.context_id()));
}

void SettingsManager::on_mode_filter_high_not_found() {
    p_mode_filter_high.set_quiet(mode_group_filter_high_default((ModeGroup)p_mode_filter_high.context_id()));
}

// The rows of every mode never written: the value in use stays, and zoom_step_scope saves it there
void SettingsManager::on_mode_freq_step_not_found() {
    if (p_mode_freq_step.context_id() != MODE_ALL) {
        p_mode_freq_step.set_quiet(mode_group_freq_step_default((ModeGroup)p_mode_freq_step.context_id()));
    }
}

void SettingsManager::on_mode_zoom_not_found() {
    if (p_mode_zoom.context_id() != MODE_ALL) {
        p_mode_zoom.set_quiet(mode_group_zoom_default((ModeGroup)p_mode_zoom.context_id()));
    }
}

void SettingsManager::zoom_step_scope(bool at_init) {
    Parameter<int32_t> *const zs[] = {&p_mode_zoom, &p_mode_freq_step};

    if (p_zoom_step_per_mode.get()) {
        if (p_mode_zoom.context_id() != MODE_ALL) {
            return;
        }
        // Back per mode: the current mode's own values, which the shared ones never wrote over. A change made in its
        // rows just before the option went off may still be queued, and load() reads the database
        pending_writes_.flush_storage(StorageType::MODE, mode_group_id_);
        for (Parameter<int32_t> *p : zs) {
            p->set_context_id(mode_group_id_);
            p->load();
        }
        return;
    }
    if (p_mode_zoom.context_id() == MODE_ALL) {
        return;
    }
    // One zoom and one step for every mode. At a start, those saved for every mode. The option turned off now, or
    // those rows never written: the current mode's, the ones on screen (nothing jumps), written to the rows of every
    // mode so a restart in another mode finds them and not that mode's own
    for (Parameter<int32_t> *p : zs) {
        p->set_context_id(MODE_ALL);
        if (!at_init || p->load() == NOT_FOUND) {
            pending_writes_.write(StorageKey{StorageType::MODE, MODE_ALL, p->db_name()}, p->get());
        }
    }
}

void SettingsManager::zoom_step_scope_cb(Subject * /*subj*/, void *user_data) {
    static_cast<SettingsManager *>(user_data)->zoom_step_scope(false);
}

void SettingsManager::switch_band(int new_band_id, bool implicit) {
    // Re-entrancy guard: an explicit switch triggered by setting p_band_id calls
    // switch_band(implicit=false). If that happens from inside this call (the
    // implicit path sets p_band_id below, firing the observer), return early so
    // exactly one load_band_switch runs with the correct `implicit` flag.
    if (band_switch_active_) {
        return;
    }
    band_switch_active_ = true;

    // Batch the whole switch into a single notification round: every load
    // (set_quiet) and recompute below records its subject in the suppression
    // queue instead of firing callbacks one by one. The block scope ends BEFORE
    // band_switch_active_ is cleared, so the p_band_id observer deferred by the
    // suppressed set() below is caught by the re-entrancy guard instead of
    // running a second explicit switch.
    {
        NotifySuppressGuard guard;

        // Save pending writes of the current band before switching context.
        pending_writes_.flush_storage(StorageType::BAND, band_id_);

        band_id_ = new_band_id;

        // Persist the current band so a restart returns to it.
        p_band_id.set(new_band_id);

        set_band_context(band_id_);

        load_band_switch(band_id_, implicit);

        cp_fg_freq.recompute();
        cp_cur_mode.recompute();
        cp_cur_att.recompute();
        cp_cur_pre.recompute();
        cp_cur_agc.recompute();
        cp_bg_freq.recompute();

        // The band the active VFO is on, for independent VFOs (vfo_toggle)
        (p_band_current_vfo.get() == RADIO_VFO_A ? p_vfoa_band : p_vfob_band).set(new_band_id);

        // The VFO in use, saved with the band it is now in use on: a switch keeps it without writing it, so each band
        // kept the VFO in use when it was last left there, and a restart came back on that one (B tuned from 20m
        // into the gap above CB C, or the VFO key's switch to B's band with independent VFOs: A after the restart)
        pending_writes_.write(StorageKey{StorageType::BAND, band_id_, p_band_current_vfo.db_name()},
                              p_band_current_vfo.get());
    }

    band_switch_active_ = false;
}

void SettingsManager::vfo_toggle() {
    const bool    on_a  = p_band_current_vfo.get() == RADIO_VFO_A;
    const int32_t other = (on_a ? p_vfob_band : p_vfoa_band).get();

    (on_a ? p_vfoa_band : p_vfob_band).set(band_id_);
    p_band_current_vfo.set(on_a ? RADIO_VFO_B : RADIO_VFO_A);
    if (p_indep_vfo.get() && other != BAND_UNDEFINED && other != band_id_ && band_usable(other)) {
        p_band_id.set(other);   /* explicit switch: the VFO now in use from that band, the other kept (load_band_vfo) */
    } else if (p_band_tx_shift.get() && p_band_split.get()) {
        band_follow(p_band_vfoa_freq.get());    /* a TX shift: the band is A's, B = A + the shift can lie beyond it */
    } else {
        band_follow(fg_freq_get());   /* the VFO now active was tuned into another band while inactive */
    }
}

// A band the radio can use, for the other VFO's own band (vfo_toggle, load_band_all): a 2 m left there by v0.3.5 on
// an X6200 with the transverters and the WR12 off is not (load_band_vfo would keep its 145.500, and the base be sent it)
bool SettingsManager::band_usable(int32_t id) const {
    BandInfoLoadResult b = BandsTable::get_by_id(id);
    return b.rc == SUCCESS && band_use(b.value.start_freq, b.value.stop_freq) != BAND_USE_NONE;
}

void SettingsManager::tx_shift_follow() {
    const int32_t shift = p_band_tx_shift.get();

    if (shift == 0 || !p_band_split.get() || tx_shift_busy_) {
        return;
    }
    tx_shift_busy_ = true;
    // The mode too: in split the base transmits on the other VFO in that VFO's own mode, so a B left in NFM sent
    // FM on an SSB band (bench, 2026-10-26)
    if (p_band_current_vfo.get() == RADIO_VFO_A) {
        p_band_vfob_freq.set(p_band_vfoa_freq.get() + shift);
        p_band_vfob_mode.set(p_band_vfoa_mode.get());
    } else {
        p_band_vfoa_freq.set(p_band_vfob_freq.get() - shift);
        p_band_vfoa_mode.set(p_band_vfob_mode.get());
    }
    // Independent VFOs: the other VFO is on this band too, so a VFO switch stays here (the reverse) instead of
    // loading the band that VFO was on before
    Parameter<int32_t> &other = p_band_current_vfo.get() == RADIO_VFO_A ? p_vfob_band : p_vfoa_band;
    if (other.get() != band_id_) {
        other.set(band_id_);
    }
    tx_shift_busy_ = false;
}

void SettingsManager::tx_shift_cb(Subject * /*subj*/, void *user_data) {
    static_cast<SettingsManager *>(user_data)->tx_shift_follow();
}

void SettingsManager::tx_shift_set(int32_t hz) {
    p_band_tx_shift.set(hz);
    p_band_split.set(hz != 0);
}

void SettingsManager::switch_mode(ModeGroup new_group_id) {
    // Batch the mode switch into a single notification round: the mode-param
    // loads (set_quiet) and the freq/filter recomputes below are coalesced so
    // every changed subject fires exactly one callback.
    if (new_group_id == mode_group_id_) {
        return;
    }
    NotifySuppressGuard guard;

    pending_writes_.flush_storage(StorageType::MODE, mode_group_id_);

    mode_group_id_ = new_group_id;

    set_mode_context(mode_group_id_);

    load_mode_all();

    // The mode context can affect computed parameters that depend on mode
    // (e.g. current_mode_id); recompute the fg frequency chain.
    cp_fg_freq.recompute();

    // Mode loads use set_quiet (no source notify), and the category may have
    // flipped, so recompute the filter chain explicitly. NOT bound to
    // cp_cur_mode: binding would recompute with stale filter values at the
    // moment the mode flips.
    cp_cur_filter_low.recompute();
    cp_cur_filter_high.recompute();
    cp_cur_filter_bw.recompute();
}

void SettingsManager::flush_all() {
    pending_writes_.flush_all();
}

void SettingsManager::flush_storage(StorageType type, int context_id) {
    pending_writes_.flush_storage(type, context_id);
}

void SettingsManager::cfg_band_vfo_copy() {
    // The other VFO comes to this band too (independent VFOs)
    (p_band_current_vfo.get() == RADIO_VFO_A ? p_vfob_band : p_vfoa_band).set(band_id_);
    if (p_band_current_vfo.get() == RADIO_VFO_A) {
        p_band_vfob_freq.set(p_band_vfoa_freq.get());
        p_band_vfob_mode.set(p_band_vfoa_mode.get());
        p_band_vfob_agc.set(p_band_vfoa_agc.get());
        p_band_vfob_att.set(p_band_vfoa_att.get());
        p_band_vfob_pre.set(p_band_vfoa_pre.get());
    } else {
        p_band_vfoa_freq.set(p_band_vfob_freq.get());
        p_band_vfoa_mode.set(p_band_vfob_mode.get());
        p_band_vfoa_agc.set(p_band_vfob_agc.get());
        p_band_vfoa_att.set(p_band_vfob_att.get());
        p_band_vfoa_pre.set(p_band_vfob_pre.get());
    }
}

void SettingsManager::start_flush_thread() {
    if (flush_thread_running_) {
        return;
    }
    flush_thread_running_ = true;
    flush_thread_         = std::thread([this]() {
        std::unique_lock<std::mutex> lock(flush_mutex_);
        while (flush_thread_running_) {
            // Wake every 3 seconds (or on notify) and persist pending changes.
            flush_cv_.wait_for(lock, std::chrono::seconds(3), [this]() { return !flush_thread_running_; });
            if (!flush_thread_running_) {
                break;
            }
            pending_writes_.flush_all();
        }
    });
}

void SettingsManager::stop_flush_thread() {
    if (!flush_thread_running_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(flush_mutex_);
        flush_thread_running_ = false;
    }
    flush_cv_.notify_all();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
}

int32_t SettingsManager::fg_freq_get() {
    // Front-panel frequency = frequency of the active VFO of the current band.
    return p_band_current_vfo.get() == 0 ? p_band_vfoa_freq.get() : p_band_vfob_freq.get();
}

void SettingsManager::fg_freq_set(int32_t freq) {
    // Clamp to the nearest hardware-usable frequency before writing back, so
    // invalid frequencies never reach the VFO params.
    freq = clamp_to_valid_hw_freq(freq);
    // Write back into the active VFO's frequency param.
    if (p_band_current_vfo.get() == 0) {
        p_band_vfoa_freq.set(freq);
    } else {
        p_band_vfob_freq.set(freq);
    }
}

int32_t SettingsManager::cur_mode_get() {
    // Current mode = mode of the active VFO of the current band.
    return p_band_current_vfo.get() == 0 ? p_band_vfoa_mode.get() : p_band_vfob_mode.get();
}

void SettingsManager::cur_mode_set(int32_t mode) {
    // Write back into the active VFO's mode param.
    if (p_band_current_vfo.get() == 0) {
        p_band_vfoa_mode.set(mode);
    } else {
        p_band_vfob_mode.set(mode);
    }
}

int32_t SettingsManager::cur_att_get() {
    return p_band_current_vfo.get() == RADIO_VFO_A ? p_band_vfoa_att.get() : p_band_vfob_att.get();
}

void SettingsManager::cur_att_set(int32_t att) {
    if (p_band_current_vfo.get() == RADIO_VFO_A) {
        p_band_vfoa_att.set(att);
    } else {
        p_band_vfob_att.set(att);
    }
}

int32_t SettingsManager::cur_pre_get() {
    return p_band_current_vfo.get() == RADIO_VFO_A ? p_band_vfoa_pre.get() : p_band_vfob_pre.get();
}

void SettingsManager::cur_pre_set(int32_t pre) {
    if (p_band_current_vfo.get() == RADIO_VFO_A) {
        p_band_vfoa_pre.set(pre);
    } else {
        p_band_vfob_pre.set(pre);
    }
}

int32_t SettingsManager::cur_agc_get() {
    return p_band_current_vfo.get() == RADIO_VFO_A ? p_band_vfoa_agc.get() : p_band_vfob_agc.get();
}

void SettingsManager::cur_agc_set(int32_t agc) {
    if (p_band_current_vfo.get() == RADIO_VFO_A) {
        p_band_vfoa_agc.set(agc);
    } else {
        p_band_vfob_agc.set(agc);
    }
}

int32_t SettingsManager::bg_freq_get() {
    return p_band_current_vfo.get() == RADIO_VFO_A ? p_band_vfob_freq.get() : p_band_vfoa_freq.get();
}

void SettingsManager::bg_freq_set(int32_t freq) {
    if (p_band_current_vfo.get() == RADIO_VFO_A) {
        p_band_vfob_freq.set(freq);
    } else {
        p_band_vfoa_freq.set(freq);
    }
}

int32_t SettingsManager::clamp_to_valid_hw_freq(int32_t freq) const {
    if (is_valid_hw_freq(freq)) {
        return freq;
    }

    // Boundaries of every hardware-usable frequency range (HF_MAX_FREQ again for one that is not there)
    const bool    wr12 = HAS_VHF_RX && p_wr12.get();
    const int32_t bounds[] = {
        HF_MIN_FREQ,                // HF lower limit
        HF_MAX_FREQ,                // HF upper limit
        HAS_VHF_RX ? VHF_RX_MIN_FREQ : HF_MAX_FREQ,     // the X6200's FM broadcast and airband
        HAS_VHF_RX ? VHF_RX_MAX_FREQ : HF_MAX_FREQ,
        wr12 ? WR12_MIN_FREQ : HF_MAX_FREQ,             // the WR12
        wr12 ? WR12_MAX_FREQ : HF_MAX_FREQ,
        p_transverter_0_shift.get() ? p_transverter_0_from.get() : HF_MAX_FREQ,    // transverter 0 [from, to], on
        p_transverter_0_shift.get() ? p_transverter_0_to.get() : HF_MAX_FREQ,
        p_transverter_1_shift.get() ? p_transverter_1_from.get() : HF_MAX_FREQ,    // transverter 1 [from, to], on
        p_transverter_1_shift.get() ? p_transverter_1_to.get() : HF_MAX_FREQ,
    };

    int32_t nearest = bounds[0];
    int64_t best = freq > bounds[0] ? static_cast<int64_t>(freq) - bounds[0] : static_cast<int64_t>(bounds[0]) - freq;
    for (const int32_t b : bounds) {
        const int64_t d = freq > b ? static_cast<int64_t>(freq) - b : static_cast<int64_t>(b) - freq;
        if (d < best) {
            best    = d;
            nearest = b;
        }
    }
    return nearest;
}

SettingsManager::FilterMode SettingsManager::filter_mode(int32_t mode) const {
    switch (mode) {
        case radio_mode_lsb:
        case radio_mode_lsb_dig:
        case radio_mode_usb:
        case radio_mode_usb_dig:
            return FilterMode::SSB;
        case radio_mode_am:
        case radio_mode_sam:
            return FilterMode::AM;
        case radio_mode_nfm:
        case radio_mode_wfm:
            return FilterMode::FM;
        case radio_mode_cw:
        case radio_mode_cwr:
            return FilterMode::CW;
        default:
            return FilterMode::SSB;
    }
}

int32_t SettingsManager::filter_low_validate(int32_t v) {
    switch (cp_cur_mode.get()) {
        case radio_mode_cw:
        case radio_mode_cwr:
        case radio_mode_am:
        case radio_mode_nfm:
        case radio_mode_sam:
        case radio_mode_wfm:
            return 0;
    }
    int high = p_mode_filter_high.get();
    int hi    = high - 1;
    if (hi < 0) {
        hi = 0;
    }
    return clip(v, 0, hi);
}

/* The widest filter the mode takes: the X6200's WFM goes to 10 kHz (its stock GUI's limit, the
 * default 8 kHz), every other mode to 6 kHz */
int32_t SettingsManager::filter_high_max() {
    return cp_cur_mode.get() == radio_mode_wfm ? 10000 : 6000;
}

int32_t SettingsManager::filter_high_validate(int32_t v) {
    int max = filter_high_max();
    int low = clip(p_mode_filter_low.get(), 0, max - 1);
    return clip(v, low + 1, max);
}

int32_t SettingsManager::cur_filter_low_compute() {
    // Current mode category drives the edge mapping. The filter params are
    // MODE-scoped to cp_cur_mode.get() (matches mode_id_ at steady state).
    int32_t low;
    switch (filter_mode(cp_cur_mode.get())) {
        case FilterMode::AM:
        case FilterMode::FM:
            return 0;
        case FilterMode::CW:
            // Low can't be negative
            low = p_key_tone.get() - p_mode_filter_high.get() / 2;
            return std::max(0, low);
        case FilterMode::SSB:
        default:
            return p_mode_filter_low.get();
    }
}

int32_t SettingsManager::cur_filter_high_compute() {
    int32_t low;
    switch (filter_mode(cp_cur_mode.get())) {
        case FilterMode::SSB:
        case FilterMode::AM:
        case FilterMode::FM:
        default:
            return p_mode_filter_high.get();
        case FilterMode::CW:
            low = p_key_tone.get() - p_mode_filter_high.get() / 2;
            low = std::max(0, low);
            return low + p_mode_filter_high.get();
    }
}

int32_t SettingsManager::cur_filter_bw_compute() {
    return cp_cur_filter_high.get() - cp_cur_filter_low.get();
}

void SettingsManager::cur_filter_low_reverse(int32_t v) {
    switch (filter_mode(cp_cur_mode.get())) {
        case FilterMode::SSB:
            p_mode_filter_low.set(v);
            break;
        case FilterMode::AM:
        case FilterMode::FM:
            // no-op: low is always 0 for AM/FM.
            break;
        case FilterMode::CW:
            // filter_high is centred on key_tone: low = key_tone - high/2.
            p_mode_filter_high.set(2 * (p_key_tone.get() - v));
            break;
    }
}

void SettingsManager::cur_filter_high_reverse(int32_t v) {
    switch (filter_mode(cp_cur_mode.get())) {
        case FilterMode::SSB:
        case FilterMode::AM:
        case FilterMode::FM:
            p_mode_filter_high.set(v);
            break;
        case FilterMode::CW:
            {
                /* Past twice the key tone the low edge sits at 0 and the
                 * width is the high edge itself (upstream d34f309) */
                int32_t expected_low = 2 * p_key_tone.get() - v;
                int32_t p_high_val   = 2 * (v - p_key_tone.get());

                if (expected_low < 0) {
                    p_high_val = v;
                }
                p_mode_filter_high.set(p_high_val);
            }
            break;
    }
}

void SettingsManager::cur_filter_bw_reverse(int32_t v) {
    v = clip(v, 10, filter_high_max());
    switch (filter_mode(cp_cur_mode.get())) {
        case FilterMode::AM:
        case FilterMode::FM:
            // bw == filter_high in AM/FM (low is 0).
            p_mode_filter_high.set(v);
            break;
        case FilterMode::SSB:
            {
                const int32_t mid  = (p_mode_filter_low.get() + p_mode_filter_high.get()) / 2;
                int32_t       low  = mid - v / 2;
                int32_t       high = mid + v / 2;
                if (low < 0) {
                    low  = 0;
                    high = v;
                }
                p_mode_filter_low.set(low);
                p_mode_filter_high.set(high);
                break;
            }
        case FilterMode::CW:
            // bw == filter_high in CW (the +/- offset cancels).
            p_mode_filter_high.set(v);
            break;
    }
}

void SettingsManager::switch_mode_observer_cb(Subject * /*subj*/, void *user_data) {
    // Whenever the current mode changes, switch the mode context so the
    // MODE-scoped params track the active VFO's mode. switch_mode() only loads
    // MODE params (set_quiet) and recomputes cp_fg_freq, never cp_cur_mode, so
    // this does not recurse.
    SettingsManager *mgr = static_cast<SettingsManager *>(user_data);
    auto group = mode_group((radio_mode_t)mgr->cp_cur_mode.get());
    mgr->switch_mode(group);
}

void SettingsManager::vfo_freq_change_cb(Subject *subj, void *user_data) {
    // The band follows the active VFO: when its frequency lands in another band
    // the band switches implicitly, that frequency kept (it was just tuned) and
    // the band's mode/att/pre/agc loaded. The inactive VFO's frequency is only
    // stored: a switch from it loaded the new band over the active VFO's mode
    // and over the frequency just asked for (audit B11); vfo_toggle() follows
    // it once it is the active one. Triggered from cp_fg_freq.set() (via
    // fg_freq_set -> active VFO freq .set()) and direct p_band_vfo*_freq.set().
    SettingsManager *mgr    = static_cast<SettingsManager *>(user_data);
    Subject         *active = mgr->p_band_current_vfo.get() == RADIO_VFO_A
                                  ? static_cast<Subject *>(&mgr->p_band_vfoa_freq)
                                  : static_cast<Subject *>(&mgr->p_band_vfob_freq);
    if (subj != active) {
        return;
    }
    int32_t freq = static_cast<SubjectT<int32_t> *>(subj)->get();

    // REV with a TX shift: the band of VFO A, which tx_shift_follow is about to put at B - the shift. B itself can
    // lie beyond the band (10 m + 500 kHz from 29.500), and following it went into the gap above and lost the shift
    if (subj == &mgr->p_band_vfob_freq && mgr->p_band_tx_shift.get() && mgr->p_band_split.get()) {
        freq = std::max(freq - mgr->p_band_tx_shift.get(), HF_MIN_FREQ);
    }
    mgr->band_follow(freq);
}

void SettingsManager::band_follow(int32_t freq) {
    BandInfoLoadResult result = BandsTable::get_by_freq(static_cast<uint32_t>(freq));
    if (result.rc == SUCCESS && result.value.id != band_id_) {
        switch_band(result.value.id, true);
    }
}

void SettingsManager::switch_band_observer_cb(Subject * /*subj*/, void *user_data) {
    // Whenever p_band_id changes, switch bands explicitly (implicit=false): the
    // caller set p_band_id to select a new band. The active VFO reference and
    // both VFO frequencies are loaded from the new band. switch_band() sets
    // p_band_id to the same value (no-op), and the re-entrancy guard prevents a
    // nested switch when this fires from the implicit path.
    SettingsManager *mgr = static_cast<SettingsManager *>(user_data);
    mgr->switch_band(mgr->p_band_id.get(), false);
}

ModeGroup SettingsManager::mode_group(radio_mode_t mode) {
    switch (mode) {
        case radio_mode_lsb:
        case radio_mode_usb:
            return MODE_GROUP_SSB;

        case radio_mode_lsb_dig:
        case radio_mode_usb_dig:
            return MODE_GROUP_DIGI;

        case radio_mode_cw:
        case radio_mode_cwr:
            return MODE_GROUP_CW;

        case radio_mode_sam:
            return MODE_GROUP_AM;

        default:
            return static_cast<ModeGroup>(mode);
    }
}

int32_t SettingsManager::mode_group_filter_low_default(ModeGroup group) {
    switch (group) {
        case MODE_GROUP_SSB:
        case MODE_GROUP_DIGI:
            return 50;

        default:
            return 0;
    }
}

int32_t SettingsManager::mode_group_filter_high_default(ModeGroup group) {
    switch (group) {
        case MODE_GROUP_SSB:
        case MODE_GROUP_DIGI:
            return 2950;
        case MODE_GROUP_CW:
            return 500;
        case MODE_GROUP_WFM:
            return 8000;
        default:
            return 4000;
    }
}

int32_t SettingsManager::mode_group_freq_step_default(ModeGroup group) {
    switch (group) {
        case MODE_GROUP_SSB:
        case MODE_GROUP_DIGI:
            return 500;
        case MODE_GROUP_CW:
            return 10;
        default:
            return 1000;
    }
}

int32_t SettingsManager::mode_group_zoom_default(ModeGroup group) {
    switch (group) {
        case MODE_GROUP_CW:
            return 4;
        default:
            return 1;
    }
}

void SettingsManager::set_band_context(int band_id) {
    // Unified band registry now includes p_band_dac_offset (float), so the
    // explicit set_context_id call is no longer needed.
    for (ParamBase *p : band_params_) {
        p->set_context_id(band_id);
    }
    // The four VFO params are not in the generic registry (see load_band_vfo),
    // but they still need the band context so their deferred writes target the
    // right band.
    p_band_vfoa_freq.set_context_id(band_id);
    p_band_vfob_freq.set_context_id(band_id);
    p_band_vfoa_mode.set_context_id(band_id);
    p_band_vfob_mode.set_context_id(band_id);
    p_band_vfoa_att.set_context_id(band_id);
    p_band_vfob_att.set_context_id(band_id);
    p_band_vfoa_pre.set_context_id(band_id);
    p_band_vfob_pre.set_context_id(band_id);
    p_band_vfoa_agc.set_context_id(band_id);
    p_band_vfob_agc.set_context_id(band_id);
}

void SettingsManager::set_mode_context(int mode_id) {
    // The zoom and the step of every mode stay with every mode (zoom_step_scope)
    for (ParamBase *p : mode_params_) {
        if (p->context_id() != MODE_ALL) {
            p->set_context_id(mode_id);
        }
    }
}

void SettingsManager::load_band_all(int band_id) {
    for (ParamBase *p : band_params_) {
        p->load(band_id);
    }

    // VFO params are handled in explicit order (restore/clamp with sibling
    // dependency). Initial full load restores nothing (no implicit skip).
    // Independent VFOs: the other VFO as it was on its own band, where the VFO key takes it back, loaded from there
    // first and then kept as a band key keeps it (not where this band has split on). Loaded from this band, a restart
    // showed this band's other VFO; kept from nothing, the GUI's own 14.100 or 14.150
    const int32_t own   = (p_band_current_vfo.get() == RADIO_VFO_A ? p_vfob_band : p_vfoa_band).get();
    const bool    other = p_indep_vfo.get() && own != BAND_UNDEFINED && own != band_id && band_usable(own);
    if (other) {
        load_band_vfo(own, false, false);
    }
    load_band_vfo(band_id, false, other);
}

void SettingsManager::load_mode_all() {
    // Not the zoom and step of every mode: a mode change keeps them, and their row may lag a change still queued
    for (ParamBase *p : mode_params_) {
        if (p->context_id() != MODE_ALL) {
            p->load(p->context_id());
        }
    }
}

void SettingsManager::load_band_switch(int new_band_id, bool implicit) {
    // A switch never reloads current_vfo (the active VFO reference stays). The
    // generic registry holds current_vfo + if_shift + dac_offset; this loop
    // loads if_shift and dac_offset, skipping current_vfo. The VFO params
    // (and the implicit active-VFO skip) are handled by load_band_vfo.
    for (ParamBase *p : band_params_) {
        if (p == &p_band_current_vfo) {
            continue;
        }
        p->load(new_band_id);
    }
    load_band_vfo(new_band_id, implicit, implicit || p_indep_vfo.get());
}

void SettingsManager::load_band_vfo(int band_id, bool implicit, bool keep) {
    const int active_vfo = p_band_current_vfo.get();

    // BandInfo drives the clamp/restore rules. A missing or undefined band
    // disables clamping; vfoa_freq then falls back to a default restore.
    const int32_t      default_freq = 12000000;
    BandInfoLoadResult band         = BandsTable::get_by_id(band_id);
    const bool         have_band    = (band.rc == SUCCESS) && (band.value.id != BAND_UNDEFINED);

    // On an implicit switch (the active VFO tuned across a band edge) the active
    // VFO keeps the frequency that caused the switch; its mode/att/pre/agc are
    // loaded from the new band. The inactive VFO keeps all it had (keep_a,
    // keep_b), so tuning A across a band edge does not move B; but not where the
    // new band has split on (p_band_split is loaded before): the inactive VFO is
    // then the one the radio transmits on, and comes from the band as on an
    // explicit switch (kept, 40m CW in split sent on 40m SSB's B, 7.150, not its own 7.025).
    // With independent VFOs the inactive VFO is kept on every switch, a band key's too (`keep`): it stays on its own
    // band (p_vfoa_band, p_vfob_band) until the VFO key brings that band back; loaded from the band key's band, B
    // showed what it had there, not its own. Into split the same rule as above: kept, a band key to 15m left in
    // split sent on B's 7.030 (40m), not on the B that 15m transmits on.
    const bool keep_other = keep && !p_band_split.get();
    const bool keep_a = keep_other && active_vfo == RADIO_VFO_B, keep_b = keep_other && active_vfo == RADIO_VFO_A;

    // VFOA frequency: skipped on an implicit switch when VFOA is the active VFO, or kept.
    if (!(implicit && active_vfo == RADIO_VFO_A) && !keep_a) {
        // vfoa_freq: NOT_FOUND -> band start (or the default), on what the radio can tune (4m has no opening
        // frequency and neither radio tunes 70 MHz); an out-of-range loaded value is clamped.
        int rc = p_band_vfoa_freq.load(band_id);
        if (rc == NOT_FOUND) {
            p_band_vfoa_freq.set_quiet(have_band ? clamp_to_valid_hw_freq(static_cast<int32_t>(band.value.start_freq))
                                                 : default_freq);
        } else if (rc == SUCCESS && have_band) {
            const int32_t freq = p_band_vfoa_freq.get();
            if (freq < static_cast<int32_t>(band.value.start_freq) ||
                freq > static_cast<int32_t>(band.value.stop_freq)) {
                p_band_vfoa_freq.set_quiet(clamp_to_valid_hw_freq(freq));
            }
        }
    }

    // VFOA mode/att/pre/agc: kept when VFOA is the inactive VFO kept (keep_a).
    if (!keep_a) {
        int rc = p_band_vfoa_mode.load(band_id);
        if (rc != SUCCESS) {
            p_band_vfoa_mode.set_quiet(resolve_default_mode(p_band_vfoa_freq.get(), band_id));
        }
    }
    if (!keep_a) {
        int rc;
        rc = p_band_vfoa_att.load(band_id);
        (void)rc;
        rc = p_band_vfoa_pre.load(band_id);
        (void)rc;
        rc = p_band_vfoa_agc.load(band_id);
        (void)rc;
    }

    // VFOB frequency: skipped on an implicit switch when VFOB is the active VFO, or kept.
    if (!(implicit && active_vfo == RADIO_VFO_B) && !keep_b) {
        // vfob_freq: copies the current vfoa_freq when absent; clamped like
        // vfoa_freq when loaded.
        int rc = p_band_vfob_freq.load(band_id);
        if (rc == NOT_FOUND) {
            p_band_vfob_freq.set_quiet(p_band_vfoa_freq.get());
        } else if (rc == SUCCESS && have_band) {
            const int32_t freq = p_band_vfob_freq.get();
            if (freq < static_cast<int32_t>(band.value.start_freq) ||
                freq > static_cast<int32_t>(band.value.stop_freq)) {
                p_band_vfob_freq.set_quiet(clamp_to_valid_hw_freq(freq));
            }
        }
    }

    // What VFOB falls back to with nothing of its own saved on the band: the VFOA value. With VFOA kept on its own
    // band (VFOB tuned across the edge), what VFOA last had on this band, else `dflt`: VFOA's mode from the band it
    // stayed on gave VFOB on 7.039 (40m CW) the LSB of 40m SSB, and CW after a VFO key back to 7.050
    auto a_here = [&](Parameter<int32_t> &a, int32_t dflt) {
        if (!keep_a) return a.get();
        std::optional<int32_t> v = storage_policy_for(StorageType::BAND).load_int(band_id, a.db_name());
        return v ? *v : dflt;
    };

    // VFOB mode/att/pre/agc: kept when VFOB is the inactive VFO kept (keep_b);
    // else they fall back to a_here() when
    // absent (mode copied, att/pre/agc copied on NOT_FOUND).
    if (!keep_b) {
        int rc = p_band_vfob_mode.load(band_id);
        if (rc != SUCCESS) {
            p_band_vfob_mode.set_quiet(a_here(p_band_vfoa_mode, resolve_default_mode(p_band_vfob_freq.get(), band_id)));
        }
    }
    if (!keep_b) {
        int rc;
        rc = p_band_vfob_att.load(band_id);
        if (rc == NOT_FOUND) {
            p_band_vfob_att.set_quiet(a_here(p_band_vfoa_att, p_band_vfoa_att.get()));
        }
        rc = p_band_vfob_pre.load(band_id);
        if (rc == NOT_FOUND) {
            p_band_vfob_pre.set_quiet(a_here(p_band_vfoa_pre, p_band_vfoa_pre.get()));
        }
        rc = p_band_vfob_agc.load(band_id);
        if (rc == NOT_FOUND) {
            p_band_vfob_agc.set_quiet(a_here(p_band_vfoa_agc, p_band_vfoa_agc.get()));
        }
    }
}

/* 60 m is worked in USB, though under 10 MHz (the WRC-15 band and the channels around it) */
int32_t SettingsManager::default_mode(int32_t freq) const {
    if (band_use(freq, freq) == BAND_USE_RX) {
        if (freq >= VHF_RX_MIN_FREQ && freq < 108000000) return radio_mode_wfm;
        if (freq >= 108000000 && freq <= VHF_RX_MAX_FREQ) return radio_mode_am;
        return radio_mode_nfm;      /* the WR12: 2 m, 70 cm and the like */
    }
    if (freq >= 5250000 && freq <= 5450000) return radio_mode_usb;
    return freq < 10000000 ? radio_mode_lsb : radio_mode_usb;
}

/* A band (or a gap) with no mode saved yet. Broadcast bands (ids 100-199) were LSB under 10 MHz and USB above,
 * where every station is AM: it sounded garbled and hard to find (issue #9). FM broadcast and Airband (112, 113)
 * are receive bands of their own */
int32_t SettingsManager::resolve_default_mode(int32_t freq, int32_t band_id) const {
    if (band_id >= 100 && band_id < 200 && band_use(freq, freq) != BAND_USE_RX) return radio_mode_am;
    return default_mode(freq);
}

std::string SettingsManager::make_default_encoder_bind() {
    std::string s(CTRL_FAST_ACCESS_LAST, static_cast<char>(ENCODER_BIND_NONE));

    s[CTRL_VOL]         = static_cast<char>(ENCODER_BIND_VOL);
    s[CTRL_RFG]         = static_cast<char>(ENCODER_BIND_VOL);
    s[CTRL_FILTER_LOW]  = static_cast<char>(ENCODER_BIND_VOL);
    s[CTRL_FILTER_HIGH] = static_cast<char>(ENCODER_BIND_VOL);
    s[CTRL_PWR]         = static_cast<char>(ENCODER_BIND_VOL);

    s[CTRL_SPECTRUM_FACTOR] = static_cast<char>(ENCODER_BIND_MFK);
    s[CTRL_DNF]             = static_cast<char>(ENCODER_BIND_MFK);

    return s;
}

std::string SettingsManager::encoder_bind_validate(const std::string &val) {
    std::string result = val;
    if (result.size() != static_cast<size_t>(CTRL_FAST_ACCESS_LAST))
        result.resize(CTRL_FAST_ACCESS_LAST, static_cast<char>(ENCODER_BIND_NONE));
    /* These moved to Settings: no longer on the encoders */
    for (cfg_ctrl_t ctrl : {CTRL_HMIC, CTRL_MIC, CTRL_IMIC, CTRL_AGC_HANG, CTRL_AGC_KNEE, CTRL_AGC_SLOPE,
                            CTRL_VOX_GAIN, CTRL_VOX_AG, CTRL_VOX_DELAY})
        result[ctrl] = static_cast<char>(ENCODER_BIND_NONE);
    return result;
}

int32_t SettingsManager::lo_offset_compute() {
    int32_t mode = cp_cur_mode.get();
    int32_t key_tone = p_key_tone.get();
    if (mode == radio_mode_cw) {
        return -key_tone;
    }
    if (mode == radio_mode_cwr) {
        return key_tone;
    }
    return 0;
}
