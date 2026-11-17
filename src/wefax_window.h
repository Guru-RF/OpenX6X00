/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax app
 */

#pragma once

#include "app_window.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_wefax;

/* The app is open (the waterfall shows its tuning marks) */
bool dialog_wefax_is_active(void);

/* What the panel asks: the buttons' own handlers. LVGL thread; false when the app is not open. PLAN and UNPLAN: Auto
 * Plan on, and off (off when the app opens too, as its button) */
typedef enum { WEFAX_RC_AUTO, WEFAX_RC_CONT, WEFAX_RC_SAVE, WEFAX_RC_CLEAR, WEFAX_RC_PLAN, WEFAX_RC_UNPLAN } wefax_rc_t;

bool wefax_window_remote(wefax_rc_t what);

/* The app as the browser's WeFax page shows it ("wefax {...}"). Any thread */
typedef struct {
    bool     open;
    bool     auto_mode;     /* RX Auto (start and stop tones), else Cont */
    bool     fax_on;        /* a fax is coming in (Auto: confirmed by its start tone) */
    bool     tx;
    int      tx_pct;
    uint32_t gen;           /* a new picture: the kept rows started over */
    uint32_t rows;          /* rows kept of this picture */
    int32_t  freq;          /* the station's frequency (dial + audio centre), Hz */
    char     station[40];   /* its name in the app's list, "" for none */
    char     saved[48];     /* the last picture written to /mnt/wefax, its file name */
    uint32_t saves;         /* pictures written since the GUI started */
    bool     plan;          /* Auto Plan on */
    char     plan_next[48]; /* its next slot, "HH:MM station kHz", "" for none */
} wefax_status_t;

void wefax_window_status(wefax_status_t *s);

#ifdef __cplusplus
}
#endif
