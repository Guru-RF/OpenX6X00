/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the APP menu as a grid of icons (apps_menu.c)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "dialog.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The grid: every app of the APP button pages as a tile with an icon and a line about it.
 * The APP key opens it while Settings > Display > Icon menus is on, and closes it again. */
extern dialog_t *dialog_apps;

/* The apps hidden in Settings > Apps, as cfg/apps_list.h's set (bit app_id_t): they have no tile here and no
 * button on the APP pages (button_bar.cpp); everything else that opens them works as before (#351) */
uint32_t apps_menu_hidden(void);

#ifdef __cplusplus
}
#endif
