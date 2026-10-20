// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - named channels, kept in /mnt/channels.json.
//
// The file is shared with the web configurator (webserver apps.py) and with the walkie
// channels file (walkie.c), which use the same mode and AGC names.
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

#define CHANNELS_MAX        100
#define CHANNEL_NAME_MAX    32

typedef struct {
    char            name[CHANNEL_NAME_MAX];
    uint32_t        freq;
    x6100_mode_t    mode;
    x6100_agc_t     agc;
    bool            pre;
    bool            att;
} channel_t;

/* All the calls below that write the file return false when that fails, with the list in
 * memory as it was before the call */

/* The file into memory; a missing one is written with a few preset stations */
bool channels_load(void);                           /* also false on a bad file: the list stays */
bool channels_save(void);                           /* the list in memory to the file */

/* Read the file again if someone else (the web configurator) wrote it.
   Indexes taken before this call may no longer be valid */
void channels_reload_if_changed(void);

uint16_t channels_count(void);                      /* entries in memory */
const channel_t *channels_get(uint16_t i);          /* NULL past the end */

bool channels_add_current(void);                    /* the current settings, no name, at the end */
bool channels_delete(uint16_t i);                   /* the ones after it move up; saved */
bool channels_set_name(uint16_t i, const char *name);   /* saved */
bool channels_move(uint16_t i, uint16_t to);        /* memory only: the dialog saves on Drop */

/* Mode and AGC as written in the file ("USB-D", "SLOW"), and back */
const char *channels_mode_name(x6100_mode_t mode);
bool channels_mode_parse(const char *value, x6100_mode_t *mode);
const char *channels_agc_name(x6100_agc_t agc);
bool channels_agc_parse(const char *value, x6100_agc_t *agc);
