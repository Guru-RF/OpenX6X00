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
 * Run fn on the GUI thread later, with a copy of arg (arg_size bytes).
 * False: the queue is full, fn will not run; the caller says so or tries again (audit A21)
 */
bool scheduler_put(scheduler_fn_t fn, void *arg, size_t arg_size);

/**
 * The same with room kept beyond the usual bound: for what must get through when the queue
 * is flooded (an unkey, a stop)
 */
bool scheduler_put_urgent(scheduler_fn_t fn, void *arg, size_t arg_size);

/**
 * Run fn(NULL) on the GUI thread later. False: the queue is full
 */
bool scheduler_put_noargs(scheduler_fn_t fn);

/**
 * Run fn(arg) on the GUI thread and wait for it; on the GUI thread itself it runs at once.
 * arg is the caller's, not copied. False when the queue is full (fn did not run)
 */
bool scheduler_call(scheduler_fn_t fn, void *arg);

/**
 * Run what is queued, up to a budget per call; the rest waits for the next loop
 */
void scheduler_work();

#ifdef __cplusplus
}
#endif
