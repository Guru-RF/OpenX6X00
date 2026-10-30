/* Host test shim of src/cfg/cfg_api.h: the settings tx_player.c and ext_tune.c read, which the test keeps
 * (tune_test.c), and the subjects ext_tune.c subscribes to */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct ParamFloat       ParamFloat;
typedef struct ParamInt         ParamInt;
typedef struct ComputedParamInt ComputedParamInt;
typedef struct Subject          Subject;
typedef struct Observer         Observer;
typedef void (*observer_cb)(Subject *, void *);
extern ParamFloat       *cfg_pwr, *cfg_tune_pwr;
extern ParamInt         *cfg_tune_time, *cfg_wspr_tx_freq;
extern ParamInt         *cfg_band_vfoa_freq, *cfg_band_vfob_freq, *cfg_band_vfoa_mode, *cfg_band_vfob_mode,
                        *cfg_band_current_vfo, *cfg_band_split, *cfg_band_tx_shift;
extern ComputedParamInt *cfg_cur_mode;
float     param_f_get(const ParamFloat *p);
int32_t   param_i_get(const ParamInt *p);
int32_t   cparam_i_get(const ComputedParamInt *p);
bool      cfg_is_tx_freq(int32_t freq);
float     cfg_pwr_max_get(void);
Observer *subject_subscribe(Subject *subj, observer_cb fn, void *user_data);
