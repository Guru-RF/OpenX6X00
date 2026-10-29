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

band_use_t cfg_band_use(int32_t start, int32_t stop) {
    return cfg_sm.band_use(start, stop);
}

bool cfg_is_valid_hw_freq(int32_t freq) {
    return cfg_sm.is_valid_hw_freq(freq);
}

bool cfg_is_rx_only_freq(int32_t freq) {
    return cfg_sm.is_rx_only_freq(freq);
}

bool cfg_is_tx_freq(int32_t freq) {
    return cfg_sm.is_tx_freq(freq);
}

bool cfg_band_usable(void) {
    BandInfoLoadResult b = BandsTable::get_by_id(cfg_sm.current_band_id());
    return b.rc != SUCCESS || cfg_sm.band_use(b.value.start_freq, b.value.stop_freq) != BAND_USE_NONE;
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

/* The next active band above (starting at or above the frequency) or below (ending at or below it) that the radio can
 * use: 4m, and 2m and 70cm without a transverter or the WR12, are stepped over (BandsTable::next asked only for
 * the Active flag). One walk of the list: asking next() again from a band skipped could go round two bands that
 * start together */
void cfg_band_load_next(bool up) {
    std::vector<BandInfo> all      = BandsTable::all_bands();      /* by start_freq */
    int32_t               cur_freq = cfg_sm.cp_fg_freq.get();
    int32_t               cur_id   = cfg_sm.current_band_id();
    int32_t               id       = BAND_UNDEFINED;

    for (size_t k = 0; k < all.size() && id == BAND_UNDEFINED; k++) {
        const BandInfo &b = all[up ? k : all.size() - 1 - k];

        if (b.type == BAND_ACTIVE && b.id != cur_id
            && (up ? (int32_t)b.start_freq >= cur_freq : (int32_t)b.stop_freq <= cur_freq)
            && cfg_sm.band_use(b.start_freq, b.stop_freq) != BAND_USE_NONE)
            id = b.id;
    }
    if (id != BAND_UNDEFINED) {
        cfg_sm.p_band_id.set(id);
    }
}

void cfg_pwr_set(float watts) {
    cfg_sm.p_pwr.set(watts);
}

void cfg_pwr_max_set(float watts) {
    cfg_sm.pwr_max_.store(watts);
}

void cfg_vfo_toggle(void) {
    cfg_sm.vfo_toggle();
}

int32_t cfg_default_mode(int32_t freq) {
    return cfg_sm.default_mode(freq);
}

int cfg_bands_active(int32_t *start, int32_t *stop, char (*names)[16], int max) {
    return cfg_bands_active_ids(NULL, start, stop, names, max);
}

int cfg_bands_active_ids(int32_t *ids, int32_t *start, int32_t *stop, char (*names)[16], int max) {
    std::vector<BandInfo> all = BandsTable::all_bands();
    int                   n   = 0;

    std::sort(all.begin(), all.end(), [](const BandInfo &a, const BandInfo &b) { return a.start_freq < b.start_freq; });
    for (const BandInfo &b : all) {
        if (b.type != BAND_ACTIVE || n >= max || cfg_sm.band_use(b.start_freq, b.stop_freq) == BAND_USE_NONE)
            continue;
        if (ids) ids[n] = b.id;
        start[n] = (int32_t)b.start_freq;
        stop[n]  = (int32_t)b.stop_freq;
        snprintf(names[n], 16, "%s", b.name.c_str());
        n++;
    }
    return n;
}

int cfg_bands_follow_region(int region) {
    int from = cfg_sm.p_band_edges_region.get();

    if (region == from || region < 1 || region > 3) return 0;
    int changed = BandsTable::follow_region(from, region);
    cfg_sm.p_band_edges_region.set(region);
    return changed;
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