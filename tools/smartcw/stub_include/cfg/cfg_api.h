/* Host test shim (tools/smartcw): the settings paddle_keyer.c reads, as variables the test sets */
#pragma once

#include <stdatomic.h>
#include <stdint.h>

extern _Atomic int cfg_cur_mode, cfg_key_mode, cfg_paddle_swap, cfg_iambic_mode, cfg_key_speed, cfg_paddle_serial;
extern _Atomic int cfg_paddle_map[4];
extern float       cfg_key_ratio;

#define cparam_i_get(p) atomic_load(&(p))
#define param_i_get(p)  atomic_load(&(p))
#define param_f_get(p)  (p)
