/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Frequency bookmarks: a list over an app, browsed with the MFK; a press
 *  tunes the VFO (USB) so the station lands on the decoder's audio centre.
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *group; /* non-NULL: a group header row, the other fields unused */
    int32_t     freq_hz;
    const char *name;
    const char *note;
    bool        lsb;   /* tuned in LSB (SSTV below 10 MHz), else USB */
} freq_list_item_t;

/* Opens the list on parent. audio_hz is the decoder's audio centre: the
 * dial is freq_hz - audio_hz. on_close runs when the list closes. */
void freq_list_open(lv_obj_t *parent, const char *title, const freq_list_item_t *items, int count,
                    int32_t audio_hz, void (*on_close)(void));
void freq_list_close(void);
bool freq_list_is_open(void);

/* Tunes to an entry as choosing it in the list does (LVGL thread) */
void freq_list_tune(const freq_list_item_t *it, int32_t audio_hz);

/* The entry whose dial is the closest to vfo, its dial in *dial; NULL for an empty list */
const freq_list_item_t *freq_list_base(const freq_list_item_t *items, int count, int32_t audio_hz, int32_t vfo,
                                       int32_t *dial);
