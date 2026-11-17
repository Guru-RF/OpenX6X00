/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax full screen viewer: a saved chart over the whole screen, the frequency strip and the buttons
 *  gone. The MFK's press goes whole -> x2 -> 1:1 -> whole, the MFK moves up and down, VOL left and right; ESC goes
 *  back. A small map in a corner shows where the screen is.
 *
 *  Fix: the whole chart over the screen but the buttons' row, Tilt on the MFK, Align on VOL, the MFK's press finds
 *  the tilt (wefax/tilt.h); upright guide lines show when the frame stands straight. Save writes the chart and its
 *  preview. From OlivLoze's fork (F4OLI).
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dir/name over the whole screen, title on its top line; fix: straight into Fix. on_close when it goes, the keys
 * given back there */
bool wefax_viewer_open(const char *dir, const char *name, const char *title, bool fix, void (*on_close)(void));
void wefax_viewer_close(void);
bool wefax_viewer_is_open(void);

/* Fix: the chart's Tilt and Align, the buttons' row left visible */
void wefax_viewer_fix(void);
bool wefax_viewer_fixing(void);
/* The tilt that straightens it, searched by a worker thread */
void wefax_viewer_auto_tilt(void);
/* Back to the chart as saved */
void wefax_viewer_undo(void);
/* Leaving Fix: Cancel closes without writing, Save writes it (a worker) then closes */
void wefax_viewer_cancel(void);
void wefax_viewer_save(void);

#ifdef __cplusplus
}
#endif
