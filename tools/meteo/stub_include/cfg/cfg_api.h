/* tools/meteo: APP > Weather > Notices as meteo_alerts_test.c sets it */
#pragma once
#include <stdint.h>
typedef struct ParamInt {
    int32_t v;
} ParamInt;
extern ParamInt *cfg_meteo_alerts;
int32_t param_i_get(const ParamInt *p);
