/* Host test shim (tools/smartcw): the settings paddle_keyer.c reads, as variables the test sets */
#pragma once

#include <stdatomic.h>
#include <stdint.h>

extern _Atomic int cfg_cur_mode, cfg_key_mode, cfg_paddle_swap, cfg_iambic_mode, cfg_key_speed, cfg_paddle_serial;
extern _Atomic int cfg_paddle_map[4];
extern float       cfg_key_ratio;

/* Every read goes through settings_read() (the test's): it tells one made before the settings are, by the keyer thread
 * on a keyboard there at boot */
void settings_read(void);

#define cparam_i_get(p) (settings_read(), atomic_load(&(p)))
#define param_i_get(p)  (settings_read(), atomic_load(&(p)))
#define param_f_get(p)  (settings_read(), (p))
