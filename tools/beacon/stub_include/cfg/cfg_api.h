/* Host test shim of src/cfg/cfg_api.h: the KEY settings cw_encoder.c reads, which the test keeps (beacon_test.c), and
 * the subjects QRSS3 watches */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct ParamFloat ParamFloat;
typedef struct ParamInt   ParamInt;
typedef struct Subject    Subject;
typedef struct Observer   Observer;
typedef void (*observer_cb)(Subject *, void *);
extern ParamInt   *cfg_key_speed, *cfg_key_train, *cfg_qsk_time;
extern ParamFloat *cfg_key_ratio;
extern ParamInt   *cfg_band_vfoa_freq, *cfg_band_vfob_freq, *cfg_band_vfoa_mode, *cfg_band_vfob_mode,
                  *cfg_band_current_vfo, *cfg_band_split, *cfg_band_tx_shift, *cfg_xit;
int32_t   param_i_get(const ParamInt *p);
float     param_f_get(const ParamFloat *p);
Observer *subject_subscribe(Subject *subj, observer_cb fn, void *user_data);
