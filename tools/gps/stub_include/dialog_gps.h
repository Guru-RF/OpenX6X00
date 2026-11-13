/* Host test shim (tools/gps): the GPS window, open or not as the test says */
#pragma once

#include <stdbool.h>

#include "lvgl/lvgl.h"

typedef struct {
    lv_obj_t *obj;
    bool      run;
} dialog_t;

extern dialog_t *dialog_gps;
