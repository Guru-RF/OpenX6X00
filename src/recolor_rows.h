/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - coloured text in a label that wraps (recolor_rows.c)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A piece of text in one colour */
typedef struct {
    const char *text;
    size_t      len;
    uint32_t    color; /* 0xRRGGBB */
} recolor_run_t;

/* The runs one after the other as the text of a label with recolor on, wrapping at width (the label's content width)
 * in font with letter space ls: cut into the rows LVGL wraps it in, each row ended by a '\n' and opening its own
 * colours. LVGL 8 forgets the colour at the start of every row it draws, and wraps the text the same way: one colour
 * run on over the end of a row lost its colour there, and its closing '#' then took the next run's '#' as a '#' to
 * show, so "#rrggbb " showed as text (#269). A '#' (it would end the colour) or a control character shows as a space.
 * Writes at most size bytes, the last a 0, and never half a colour or half a letter; returns the length */
size_t recolor_rows(char *buf, size_t size, const recolor_run_t *runs, int n, const lv_font_t *font, lv_coord_t ls,
                    lv_coord_t width);

#ifdef __cplusplus
}
#endif
