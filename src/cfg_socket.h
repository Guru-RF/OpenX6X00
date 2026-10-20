/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - the web configurator's local socket (cfg_socket.cpp)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* UNIX socket /tmp/x6100_cfg.sock: the Display and Ethernet pages of the web configurator read and
 * set the screen options and the Ethernet address through it. Local only, nothing on the network. */
void cfg_socket_init(void);

#ifdef __cplusplus
}
#endif
