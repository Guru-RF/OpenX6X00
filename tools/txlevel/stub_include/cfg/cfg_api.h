/* Host test shim of src/cfg/cfg_api.h: the settings tx_player.c reads */
#pragma once
typedef struct ParamFloat ParamFloat;
extern ParamFloat *cfg_pwr;
float param_f_get(const ParamFloat *p);
float cfg_pwr_max_get(void);
