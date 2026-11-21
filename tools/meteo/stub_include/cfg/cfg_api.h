/* tools/meteo: APP > Weather > Notices as meteo_alerts_test.c sets it */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct ParamInt {
    int32_t v;
} ParamInt;
extern ParamInt *cfg_meteo_alerts;
int32_t param_i_get(const ParamInt *p);
/* Weather from and Weather place, which meteo.c reads: where the radio is, nothing typed */
typedef struct ParamText ParamText;
extern ParamInt  *cfg_meteo_from;
extern ParamText *cfg_meteo_place;
size_t param_t_get(const ParamText *p, char *out, size_t size);
/* Weather units, which meteo.c reads for the notices' words (METEO_UNITS_*) */
extern ParamInt  *cfg_meteo_units;
