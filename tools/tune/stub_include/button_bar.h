/* Host test shim of src/button_bar.h: the Ext tune button, which ext_tune.c marks while it tunes */
#pragma once
#include <stdbool.h>
#include "lvgl/lvgl.h"
typedef struct {
    bool mark;
} button_data_t;
extern button_data_t btn_ext_tune;
void buttons_mark(button_data_t *b, bool on);
void buttons_refresh(button_data_t *b);
