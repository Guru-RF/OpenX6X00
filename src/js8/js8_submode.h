/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 speeds for C code
 *
 *  Values follow desktop JS8Call's submode table (JS8Submode.cpp).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { /* also the index into the speed table */
    JS8_SPEED_NORMAL = 0,
    JS8_SPEED_FAST   = 1,
    JS8_SPEED_TURBO  = 2,
    JS8_SPEED_SLOW   = 3,
    JS8_SPEED_COUNT  = 4, /* how many speeds there are, not a speed */
} js8_speed_t;

/* Out-of-range speeds are treated as Normal by every function here. */
const char *js8_speed_name(js8_speed_t speed);           /* "Normal", "Fast", "Turbo", "Slow" */
char        js8_speed_letter(js8_speed_t speed);         /* first letter of the name */
int         js8_speed_bandwidth_hz(js8_speed_t speed);   /* width of the 8 tones */
int         js8_speed_period_s(js8_speed_t speed);       /* length of a TX/RX slot */
int         js8_speed_rx_threshold_hz(js8_speed_t speed); /* offsets closer than this are one station */
int         js8_speed_max_offset_hz(js8_speed_t speed);  /* highest offset that keeps the top tone under 3 kHz */
bool        js8_speed_heartbeats(js8_speed_t speed);     /* Turbo sends none, like desktop */
int         js8_speed_rx_mask(js8_speed_t speed);        /* its JS8_SUBMODE_* bit for js8_rx_create() */
int         js8_speed_submode(js8_speed_t speed);        /* desktop submode number: 0, 1, 2 or 4 */
/* The speed of a decode, from js8_rx_msg_t.submode (0/1/2/4); anything else is Normal. */
js8_speed_t js8_speed_from_submode(int submode_number);

#ifdef __cplusplus
}
#endif
