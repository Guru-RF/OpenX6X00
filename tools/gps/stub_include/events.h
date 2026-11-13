/* Host test shim (tools/gps): the GPS window's events, counted by the test */
#pragma once

#include "lvgl/lvgl.h"

#define EVENT_GPS   1

void event_send(lv_obj_t *obj, int event_code, void *param);
