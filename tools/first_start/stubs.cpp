/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Stand-ins for what src/cfg calls outside it (tools/first_start): LVGL's log (the test prints its own), its async
 *  calls (the delayed observers, none of them the test's, never run) and the GUI thread's check (the test is it).
 */

#include "lvgl.h"
#include "scheduler.h"

extern "C" {

void _lv_log_add(lv_log_level_t level, const char *file, int line, const char *func, const char *format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)func;
    (void)format;
}

lv_res_t lv_async_call(lv_async_cb_t async_xcb, void *user_data) {
    (void)async_xcb;
    (void)user_data;
    return LV_RES_OK;
}

lv_res_t lv_async_call_cancel(lv_async_cb_t async_xcb, void *user_data) {
    (void)async_xcb;
    (void)user_data;
    return LV_RES_OK;
}

bool scheduler_on_gui_thread() {
    return true;
}
}
