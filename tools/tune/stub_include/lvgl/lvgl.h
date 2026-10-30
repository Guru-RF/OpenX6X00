/* Host test shim for the tune: the LVGL macros tx_player.c uses */
#pragma once
#define LV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define LV_MAX(a, b) ((a) > (b) ? (a) : (b))
