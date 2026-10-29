/* Host test shim of src/scheduler.h: no LVGL thread here, stubs.c runs the work at once */
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef void (*scheduler_fn_t)(void *);
bool scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size);
