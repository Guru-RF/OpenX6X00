/* Host test shim (tools/panel_stripes): what panel_stripes.c takes of LVGL, the test's own */
#pragma once

#include <stdint.h>

typedef struct _lv_timer_t lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *);

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data);

void test_log(const char *fmt, ...);
#define LV_LOG_USER(...) test_log(__VA_ARGS__)
