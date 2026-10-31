/* Host test shim of src/cfg/cfg_api.h: the two settings digi_textlog.c reads, which the test keeps
 * (textlog_test.c) */
#pragma once
#include <stdint.h>
typedef struct ParamInt ParamInt;
extern ParamInt *cfg_textlog, *cfg_textlog_mb;
int32_t param_i_get(const ParamInt *p);
