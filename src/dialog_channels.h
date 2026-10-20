// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - the CHANNELS window (Channel Memory) and channel recall by key.
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#ifndef X6100_DIALOG_CHANNELS_H
#define X6100_DIALOG_CHANNELS_H

#include <stdbool.h>     /* the bool answers */
#include <stdint.h>      /* int8_t step */

#include "dialog.h"      /* dialog_t */

extern dialog_t *dialog_channels;       /* opened by the hand mic V/M key */

/* Tune the channel before (step < 0) or after (> 0) the last one recalled, wrapping around.
   With nothing recalled yet: the last channel going back, the first going on */
bool dialog_channels_recall_relative(int8_t step);

const char *dialog_channels_last_recalled_name(void);   /* "CHANNEL n" if unnamed, "" if none */

/* Called by a timer: pick up changes from the web configurator, not while the window is open */
void dialog_channels_poll(void);

#endif /* X6100_DIALOG_CHANNELS_H */
