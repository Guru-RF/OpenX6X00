/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The browser remote's sessions as the TCI server asks about them (remote_server.c):
 *  Tailscale Funnel (#58) puts the Panel on the internet, its sound and microphone on TCI, and a
 *  connection from there is taken only with the token of a browser paired at home.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A wrong token presented over Funnel (addr: the browser's address, from X-Forwarded-For): after ten within ten
 * minutes, Funnel's connections are refused for ten minutes, on both sockets */
void remote_server_funnel_refused(const char *addr);
bool remote_server_funnel_paused(void);

#ifdef __cplusplus
}
#endif
