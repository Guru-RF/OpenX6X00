/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax: the saved charts written and read back, and their previews. A preview is the whole chart at most
 *  WEFAX_PREVIEW_W x WEFAX_PREVIEW_H, grey, in <dir>/thumbs/<name>.prv: the Bulletin draws from it, a 2 MB PNG
 *  takes the radio too long to read for eight tiles. From OlivLoze's fork (F4OLI).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFAX_PREVIEW_W 400
#define WEFAX_PREVIEW_H 294

/* Any PNG as 8-bit grey (malloc'ed, w x h), NULL if it can't be read */
uint8_t *wefax_png_read_grey(const char *path, unsigned *w, unsigned *h);

/* img (w x h grey) written as a grey PNG with Align (px) and Tilt (wefax/tilt.h's unit) applied as the app shows
 * them: the newest (last) row stays, each older one shifts. Into path.part, then renamed: what reads the folder (the
 * Bulletin, the web page) never gets half a chart */
bool wefax_png_write_fixed(const char *path, const uint8_t *img, unsigned w, unsigned h, int align, int tilt);

/* The preview of the chart dir/name (name: wefax-....png) made from its file, or removed */
bool wefax_preview_make(const char *dir, const char *name);
void wefax_preview_remove(const char *dir, const char *name);

/* The previews whose chart is gone (deleted on the web Gallery, say), removed; how many */
int wefax_preview_sweep(const char *dir);

/* The preview of dir/name (malloc'ed, w x h), NULL when there is none yet */
uint8_t *wefax_preview_load(const char *dir, const char *name, unsigned *w, unsigned *h);

#ifdef __cplusplus
}
#endif
