/* Host test shim of src/cfg/cfg_api.h: the parameters src/winlink.c reads and sets */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct ParamInt ParamInt;
typedef struct ComputedParamInt ComputedParamInt;
typedef struct ParamFloat ParamFloat;
int32_t param_i_get(const ParamInt *p);
int32_t cparam_i_get(const ComputedParamInt *p);
float   param_f_get(const ParamFloat *p);
void    cparam_i_set(ComputedParamInt *p, int32_t value);
bool    cfg_is_valid_hw_freq(int32_t freq);
float   cfg_pwr_max_get(void);
extern ParamInt *cfg_xit, *cfg_rit, *cfg_band_tx_shift, *cfg_band_split, *cfg_tx_filter_low, *cfg_tx_filter_high;
extern ComputedParamInt *cfg_fg_freq, *cfg_cur_mode, *cfg_cur_filter_low, *cfg_cur_filter_high;
extern ParamFloat *cfg_pwr;
