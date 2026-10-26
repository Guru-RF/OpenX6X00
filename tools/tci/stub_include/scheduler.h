#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef void (*scheduler_fn_t)(void *);
bool scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size);
bool scheduler_put_urgent(scheduler_fn_t fn, void *arg, size_t arg_size);
