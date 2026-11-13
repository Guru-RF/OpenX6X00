/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's base firmware written from the radio's own screen (Settings > Info > Base firmware)
 */

#pragma once

#include "app_window.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_base_update;

/* The daily check's word for Settings > Info: "1.0.9 out", "latest", or "" before any check (check.json) */
void base_update_status(char *buf, size_t n);

/* A newer release the daily check found said on the screen once (a minute after the start, then every ten) */
void base_update_watch_init(void);

#ifdef __cplusplus
}
#endif
