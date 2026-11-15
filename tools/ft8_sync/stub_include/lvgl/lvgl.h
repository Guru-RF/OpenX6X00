/* Host test shim: worker.c logs through LVGL, here nowhere */
#pragma once
#define LV_LOG_INFO(...)  ((void)0)
#define LV_LOG_WARN(...)  ((void)0)
#define LV_LOG_ERROR(...) ((void)0)
