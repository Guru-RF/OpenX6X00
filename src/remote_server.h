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
#include "board.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_CONF         BOARD_DATA "/remote.conf"
#define REMOTE_SESSIONS     BOARD_DATA "/remote_sessions.conf"   /* the paired browsers (R3) */
#define REMOTE_DEFAULT_PORT 50002

void remote_server_init(void);

/* On/off (saved in REMOTE_CONF, the web configurator reads and writes it too) and the port */
bool remote_server_get_enabled(void);
void remote_server_set_enabled(bool on);
int  remote_server_port(void);

/* Viewers connected now, and the address of one of them (NULL without) */
int         remote_server_viewers(void);
const char *remote_server_viewer(void);

/* Sessions (R3): the pairing code, shown on the screen for two minutes and copied to buf; the
 * paired browsers as "name (admin) *, ..." (* connected) and their count; every session
 * forgotten; every socket closed (the sessions stay) */
void remote_server_pair_code(char *buf, size_t n);
int  remote_server_sessions(char *buf, size_t n);
void remote_server_forget_all(void);
void remote_server_disconnect_all(void);

/* Every area LVGL flushes to the panel (screen_rec.c calls it): kept for the viewers */
void remote_flush(const lv_area_t *area, const lv_color_t *color_p);

/* For the Panel page: a row of the FT8 app's list (a cell_data_t, LVGL thread) and the CW
 * decoder's text (any thread) */
void remote_server_ft8_row(const void *cell);
void remote_server_cw_text(const char *text);

#ifdef __cplusplus
}
#endif
