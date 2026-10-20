#include "settings_manager_api.h"

#include "db.h"
#include "settings_manager.h"

#include <algorithm>
#include <cstdio>

// SettingsManager-level operations exposed to C. cfg_api_init lives in
// cfg_api.cpp (it also fills the extern parameter pointers); these are the
// pure manager operations that mostly touch only cfg_sm.

int32_t cfg_transverter_shift_for(int32_t freq) {
    return cfg_sm.transverter_shift_for(freq);
}

bool cfg_is_valid_hw_freq(int32_t freq) {
    return cfg_sm.is_valid_hw_freq(freq);
}

void cfg_api_flush_all(void) {
    cfg_sm.flush_all();
}

void cfg_api_start_flush_thread(void) {
    cfg_sm.start_flush_thread();
}

void cfg_api_stop_flush_thread(void) {
    cfg_sm.stop_flush_thread();
}

void cfg_band_load_next(bool up) {
    int32_t            cur_freq = cfg_sm.cp_fg_freq.get();
    int32_t            cur_id   = cfg_sm.current_band_id();
    BandInfoLoadResult result   = BandsTable::next(cur_id, static_cast<uint32_t>(cur_freq), up);
    if (result.rc == SUCCESS) {
        cfg_sm.p_band_id.set(result.value.id);
    }
}

void cfg_pwr_set(float watts) {
    cfg_sm.p_pwr.set(watts);
}

void cfg_vfo_toggle(void) {
    cfg_sm.vfo_toggle();
}

int32_t cfg_default_mode(int32_t freq) {
    return freq < 10000000 ? x6100_mode_lsb : x6100_mode_usb;
}

int cfg_bands_active(int32_t *start, int32_t *stop, char (*names)[16], int max) {
    std::vector<BandInfo> all = BandsTable::all_bands();
    int                   n   = 0;

    std::sort(all.begin(), all.end(), [](const BandInfo &a, const BandInfo &b) { return a.start_freq < b.start_freq; });
    for (const BandInfo &b : all) {
        if (b.type != BAND_ACTIVE || n >= max || !cfg_sm.is_valid_hw_freq(b.start_freq))
            continue;
        start[n] = (int32_t)b.start_freq;
        stop[n]  = (int32_t)b.stop_freq;
        snprintf(names[n], 16, "%s", b.name.c_str());
        n++;
    }
    return n;
}

void cfg_band_vfo_copy(void) {
    cfg_sm.cfg_band_vfo_copy();
}

int32_t cfg_mode_change_freq_step(bool up) {
    static const uint16_t steps[] = {10, 100, 500, 1000, 5000};
    static const size_t   n       = sizeof(steps) / sizeof(steps[0]);
    int32_t               step    = cfg_sm.p_mode_freq_step.get();
    size_t                i;
    for (i = 0; i < n; i++)
        if (step == static_cast<int32_t>(steps[i]))
            break;
    i    = (i + (up ? 1 : -1) + n) % n;
    step = steps[i];
    cfg_sm.p_mode_freq_step.set(step);
    return step;
}