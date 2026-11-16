/* tools/meteo: LVGL's timer as meteo_alerts_test.c runs it */
#pragma once
#include <stdint.h>
typedef struct _lv_obj_t   lv_obj_t;     /* station.h names it */
typedef struct _lv_timer_t lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *);
lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data);
