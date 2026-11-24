/* Host test shim of src/cfg/cfg_api.h: the radio's settings src/channel_list.c reads for Add (channel_list_test.c
 * holds them) */
#pragma once
#include <stdint.h>
typedef struct ComputedParamInt ComputedParamInt;
int32_t cparam_i_get(const ComputedParamInt *p);
extern ComputedParamInt *cfg_fg_freq, *cfg_cur_mode, *cfg_cur_agc, *cfg_cur_pre, *cfg_cur_att;
