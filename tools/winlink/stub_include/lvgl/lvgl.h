/* Host test shim for src/winlink.c: the few LVGL macros it uses */
#pragma once
#include <stdio.h>
#define LV_LOG_USER(...)  do { fprintf(stderr, "[user] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LV_LOG_INFO(...)  do { fprintf(stderr, "[info] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LV_LOG_WARN(...)  do { fprintf(stderr, "[warn] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LV_LOG_ERROR(...) do { fprintf(stderr, "[err ] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LV_CLAMP(min, val, max) ((val) < (min) ? (min) : ((val) > (max) ? (max) : (val)))
#define LV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define LV_MAX(a, b) ((a) > (b) ? (a) : (b))
