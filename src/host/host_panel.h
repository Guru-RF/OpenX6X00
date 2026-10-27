/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the panel on a PC: an SDL window for the screen, the PC's keys and mouse
 *  wheel as the front keys and knobs. Only in the host build (cmake -DHOST_BUILD=ON).
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

#include "lvgl/lvgl.h"

/* The window, and the FIFOs the input drivers open as the board's devices (BOARD_DEV_*) */
void host_panel_open(void);
/* LVGL's flush into the window */
void host_panel_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px);
/* The window's last frame as the radio's SIGUSR1 dump (/tmp/screen.raw: portrait 480 x 800, XRGB8888) */
bool host_panel_dump(const char *path);
/* The mouse as a pointer, the keyboard as the GUI's USB keyboard; after the drivers opened
 * their FIFOs */
void host_panel_inputs(void);
