#pragma once

// C-compatible API for the SettingsManager-level operations (settings_manager.h):
// initialisation, deferred-write flushing, band/VFO switching, frequency-step
// cycling and hardware-frequency helpers. Parameter value accessors are in
// parameter_api.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the manager (loads global/band/mode params) and fill the extern
// globals in cfg_api.h. The caller owns the sqlite3 connection/table init
// (cfg_api_init is not handed a db handle). on_db_error is kept for signature
// compatibility; it is currently inert.
void cfg_api_init(void (*on_db_error)(const char *));

/* What the radio can do on a band or at a frequency */
typedef enum {
    BAND_USE_NONE = 0,      /* nothing: it cannot tune there */
    BAND_USE_RX,            /* receive only: the X6200's FM broadcast and airband, the WR12's 54 MHz-1 GHz */
    BAND_USE_TX,            /* receive and transmit: HF, a transverter with a shift */
} band_use_t;

// Transverter shift for a frequency: the shift of the transverter whose
// [from, to] range contains freq, or 0 when none covers it (a shift of 0 is off).
int32_t cfg_transverter_shift_for(int32_t freq);

/* What the radio can do somewhere in [start, stop] (Hz; start == stop for one frequency) */
band_use_t cfg_band_use(int32_t start, int32_t stop);
// True when freq is usable by the hardware: HF 0.1-55 MHz, a transverter's
// range, the X6200's receive ranges.
bool cfg_is_valid_hw_freq(int32_t freq);
/* Received but never transmitted on: the X6200's FM broadcast and airband, the WR12's range */
bool cfg_is_rx_only_freq(int32_t freq);
/* Transmitted on: HF, a transverter with a shift */
bool cfg_is_tx_freq(int32_t freq);
/* The band the radio is on is one it can use (band_use over its edges); true between bands */
bool cfg_band_usable(void);

// Persist all pending deferred writes immediately.
void cfg_api_flush_all(void);

// Start/stop the background deferred-save thread; wakes every ~3 s and calls
// flush_all(). Inert/no-op when the thread is already running / already stopped.
void cfg_api_start_flush_thread(void);
void cfg_api_stop_flush_thread(void);

// --- Band switching ---
// Load the next/previous band above/below current frequency.
void cfg_band_load_next(bool up);

/* TX power in W (p_pwr, 0.1..BOARD_PWR_MAX), saved and applied as Settings does it */
void cfg_pwr_set(float watts);

/* The highest TX power in W that can be set (BOARD_PWR_MAX; the X6200 on battery BOARD_PWR_MAX_BAT).
 * Only later settings are clipped to it: the power already set is left as it is. The get is the top in force: a
 * transverter's TX power limit under it too (cfg_pwr_xvtr_set) */
void cfg_pwr_max_set(float watts);
float cfg_pwr_max_get(void);

/* What cfg_pwr_xvtr_set did to PWR */
typedef enum {
    PWR_XVTR_SAME = 0,
    PWR_XVTR_HELD,          /* to the limit (down, or up after it with a new one), the PWR from before kept */
    PWR_XVTR_RESTORED,      /* off the transverter: the PWR from before back */
} pwr_xvtr_t;

/* The TX power limit of a transverter (#104), W, 0 none: of the one that covers freq (on, with a shift), and the one
 * the radio is held to tuned to rx and transmitting on tx, the lower where both have one */
float cfg_transverter_pwr_for(int32_t freq);
float cfg_transverter_pwr_cap(int32_t rx, int32_t tx);

/* The transverter limit in force (radio.c, LVGL thread), 0 none: PWR stays at or under it, and comes down to it; off
 * the transverter the PWR from before comes back. The get: the limit in force, 0 none */
pwr_xvtr_t cfg_pwr_xvtr_set(float watts);
float cfg_pwr_xvtr_get(void);

/* The transverter at freq for the screen: the name of the band list's band there ("2m"), else its number ("1") */
void cfg_transverter_name(int32_t freq, char *buf, size_t size);

/* The other VFO; with independent VFOs (p_indep_vfo) its own band comes with it */
void cfg_vfo_toggle(void);

/* The mode a frequency opens in: its sideband (LSB under 10 MHz, USB above and on 60 m), WFM and AM on the X6200's FM
 * broadcast and airband, NFM where only the WR12 receives */
int32_t cfg_default_mode(int32_t freq);

/* The active bands, lowest first: start and stop in Hz and the name of each, up to max; returns how many */
int cfg_bands_active(int32_t *start, int32_t *stop, char (*names)[16], int max);
/* The same with the bands' ids (ids may be NULL) */
int cfg_bands_active_ids(int32_t *ids, int32_t *start, int32_t *stop, char (*names)[16], int max);
/* The band plan's IARU region (1, 2, 3; the US plans 2; 0 none known, nothing moves): the band list's 160m, 80m,
 * 40m SSB and 6m still on the edges of the region before take this one's (BandsTable::follow_region). The count of
 * rows changed. LVGL thread */
int cfg_bands_follow_region(int region);
/* The IARU region whose edges the band list's 160m, 80m, 40m SSB and 6m were last given (1, the defaults', until
 * another region came): the last region the band plan had, for Auto when nothing tells now */
int cfg_bands_region(void);

// Copy active VFO (freq, mode, agc, att, pre) to inactive VFO.
void cfg_band_vfo_copy(void);

// --- Step cycling ---
// Cycle freq_step through [10, 100, 500, 1000, 5000] Hz. Returns new step.
int32_t cfg_mode_change_freq_step(bool up);

#ifdef __cplusplus
} // extern "C"
#endif