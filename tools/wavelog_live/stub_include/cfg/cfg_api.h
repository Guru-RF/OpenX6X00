/* Host test shim of src/cfg/cfg_api.h: the parameters wavelog_cat.c reads and sets */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct ParamFloat ParamFloat;
typedef struct ComputedParamInt ComputedParamInt;
float   param_f_get(const ParamFloat *p);
int32_t cparam_i_get(const ComputedParamInt *p);
void    cparam_i_set(ComputedParamInt *p, int32_t value);
bool    cfg_is_valid_hw_freq(int32_t freq);
bool    cfg_is_rx_only_freq(int32_t freq);
extern ParamFloat *cfg_pwr;
extern ComputedParamInt *cfg_fg_freq, *cfg_cur_mode;
