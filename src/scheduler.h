/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef void (* scheduler_fn_t)(void *);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The calling thread is the GUI (LVGL) thread from now on: first thing in main()
 */
void scheduler_init();

/**
 * True on the GUI thread. LVGL, the parameters and their observers belong to it
 */
bool scheduler_on_gui_thread();

/**
 * Schedule execution function in main thread
 */
void scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size);


/**
 * Schedule execution function without arguments in main thread
 */
void scheduler_put_noargs(scheduler_fn_t fn);

/**
 * Run fn(arg) on the GUI thread and wait for it; on the GUI thread itself it runs at once.
 * arg is the caller's, not copied. False when the queue is full (fn did not run)
 */
bool scheduler_call(scheduler_fn_t fn, void *arg);

/**
 * Execute scheduled functions
 */
void scheduler_work();

#ifdef __cplusplus
}
#endif
