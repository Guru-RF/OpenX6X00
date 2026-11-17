/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax full screen viewer: a saved chart over the whole screen, the frequency strip and the buttons
 *  gone. The MFK's press goes whole -> x2 -> 1:1 -> whole, the MFK moves up and down, VOL left and
 *  right; ESC or any of the five buttons goes back. A small map in a corner shows where the screen is. From
 *  OlivLoze's fork (F4OLI).
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dir/name over the whole screen; title on its top line. on_close when it goes, the keys given back there */
bool wefax_viewer_open(const char *dir, const char *name, const char *title, void (*on_close)(void));
void wefax_viewer_close(void);
bool wefax_viewer_is_open(void);

#ifdef __cplusplus
}
#endif
