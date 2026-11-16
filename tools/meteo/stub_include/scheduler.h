/* tools/meteo: the LVGL thread's queue as meteo_alerts_test.c runs it */
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef void (*scheduler_fn_t)(void *arg);
bool scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size);
