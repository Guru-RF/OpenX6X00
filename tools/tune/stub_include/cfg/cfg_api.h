/* Host test shim of src/cfg/cfg_api.h: the PWR setting, which the test keeps (tune_test.c) */
#pragma once
typedef struct ParamFloat ParamFloat;
extern ParamFloat *cfg_pwr;
float param_f_get(const ParamFloat *p);
