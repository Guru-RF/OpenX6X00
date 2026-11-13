/* Host test shim (tools/smartcw): paddle_keyer.c's log lines on the test's output */
#pragma once

#include <stdio.h>

#define LV_LOG_USER(...) (printf("  log: "), printf(__VA_ARGS__), printf("\n"))
