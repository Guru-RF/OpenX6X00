/* Host test shim of src/scheduler.h: the LVGL thread's queue, run at once by tune_test.c */
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef void (*scheduler_fn_t)(void *);
bool scheduler_put_urgent(scheduler_fn_t fn, void *arg, size_t arg_size);
