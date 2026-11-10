/* Host test shim of src/cfg/cfg_api.h: the settings tx_player.c reads, and the mode in use */
#pragma once
typedef struct ParamFloat ParamFloat;
extern ParamFloat *cfg_pwr;
float param_f_get(const ParamFloat *p);
float cfg_pwr_max_get(void);
#include <stdint.h>
typedef struct ComputedParamInt ComputedParamInt;
extern ComputedParamInt *cfg_cur_mode;
int32_t cparam_i_get(const ComputedParamInt *p);
