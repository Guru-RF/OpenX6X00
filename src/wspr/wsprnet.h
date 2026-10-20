/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - WSPRnet spot upload
 *
 *  One HTTP GET per spot to http://wsprnet.org/post (plain HTTP, the radio
 *  has no TLS), like rtlsdr-wsprd; a cycle without spots sends a status
 *  report instead. Blocking: call it from a worker thread.
 */

#pragma once

#include "wspr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the request path for one spot (spot NULL: status report). */
void wsprnet_path(char *buf, size_t size, const char *rcall, const char *rgrid, uint32_t dial_hz,
                  time_t cycle, const wspr_spot_t *spot);

/* Sends the spots of a cycle. Returns how many requests got a 200. */
int wsprnet_post(const char *rcall, const char *rgrid, uint32_t dial_hz, time_t cycle,
                 const wspr_spot_t *spots, int n);

#ifdef __cplusplus
}
#endif
