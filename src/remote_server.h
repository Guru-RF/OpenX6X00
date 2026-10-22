/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Browser remote: the screen streamed as JPEG frames over a WebSocket, the front
 *  keys and the three knobs driven back (remote_server.c). The web configurator's Remote page
 *  is its client; audio goes over the TCI server (tci_server.c).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_CONF         "/mnt/remote.conf"
#define REMOTE_DEFAULT_PORT 50002

void remote_server_init(void);

/* On/off (saved in REMOTE_CONF, the web configurator reads and writes it too) and the port */
bool remote_server_get_enabled(void);
void remote_server_set_enabled(bool on);
int  remote_server_port(void);

/* Viewers connected now, and the address of one of them (NULL without) */
int         remote_server_viewers(void);
const char *remote_server_viewer(void);

/* Every area LVGL flushes to the panel (screen_rec.c calls it): kept for the viewers */
void remote_flush(const lv_area_t *area, const lv_color_t *color_p);

/* For the Panel page: a row of the FT8 app's list (a cell_data_t, LVGL thread) and the CW
 * decoder's text (any thread) */
void remote_server_ft8_row(const void *cell);
void remote_server_cw_text(const char *text);

#ifdef __cplusplus
}
#endif
