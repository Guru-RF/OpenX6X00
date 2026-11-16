/* tools/meteo: APP > Weather open or not, as meteo_alerts_test.c sets it */
#pragma once
#include <stdbool.h>
typedef struct {
    bool run;
} dialog_t;
extern dialog_t *dialog_meteo;
