#pragma once
#include <stddef.h>
typedef void (*scheduler_fn_t)(void *);
void scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size);
