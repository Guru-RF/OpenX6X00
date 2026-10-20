#pragma once

// C-compatible API for digital mode (FT8/FT4) preset navigation. Replaces the
// legacy src/cfg/digital_modes.h: the functions read the current front-panel
// frequency from the global SettingsManager (cp_fg_freq), query the
// `digital_modes` table and write the found frequency/mode back through the
// computed params (cp_fg_freq / cp_cur_mode).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The digital_modes.type column, FT8 0 up to FT2 5: the numbers are stored in params.db,
// never renumber them.
typedef enum {
    CFG_DIG_TYPE_FT8,
    CFG_DIG_TYPE_FT4,
    CFG_DIG_TYPE_JS8 = 2,           /* JS8Call dial frequencies */
    CFG_DIG_TYPE_JS8_GHOSTNET = 3,  /* JS8 on the GhostNet nets */
    CFG_DIG_TYPE_WSPR,
    CFG_DIG_TYPE_FT2,   /* 5 (upstream's 4 is WSPR here) */
} cfg_digital_type_t;

// Load the next (dir > 0), closest (dir == 0) or previous (dir < 0) digital
// mode preset for `type` relative to the current front-panel frequency, and
// apply it (frequency + mode). Returns true on success; on NOT_FOUND or a DB
// error returns false and leaves the frequency, mode and label unchanged.
bool cfg_digital_load(int8_t dir, cfg_digital_type_t type);

// Label of the last successfully loaded preset. Borrowed pointer: valid until
// the next successful cfg_digital_load() call; empty string before any
// successful load.
const char *cfg_digital_label_get(void);

// Frequency of the last successfully loaded preset (0 before any). Any thread.
int32_t cfg_digital_freq_get(void);

typedef struct {
    int32_t freq;
    char    label[32];
} cfg_digital_preset_t;

// The presets of `type` in frequency order, at most `max`: how many were written.
int cfg_digital_list(cfg_digital_type_t type, cfg_digital_preset_t *out, int max);

#ifdef __cplusplus
}
#endif
