/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Scanner: through the channels, the airband, the HF air band, CB or the band segment on the dial,
 *  stopping where something is on the air
 */

#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_scan;

/* Its lists, in the order Source goes through them */
typedef enum {
    SCAN_SRC_CHANNELS,
    SCAN_SRC_AIRBAND,
    SCAN_SRC_HF_AIR,        /* the HF Air Band app's area (dialog_aero.h) */
    SCAN_SRC_CB,            /* the CB app's plan (dialog_cb.h) */
    SCAN_SRC_SEGMENT,
    SCAN_SRC_N,
} scan_source_t;

/* The list the Scanner opens on next (the HF Air Band and CB apps' Scan), before main_screen_start_app() */
void dialog_scan_preset(scan_source_t source);

#ifdef __cplusplus
}
#endif
