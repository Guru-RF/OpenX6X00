/* tools/qso_log: LVGL's log onto stderr */
#pragma once
#include <stdio.h>
#define LV_LOG_ERROR(...) (fprintf(stderr, "error: " __VA_ARGS__), fputc('\n', stderr))
#define LV_LOG_USER(...)  (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
