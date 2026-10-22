// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *  Xiegu X6100 LVGL GUI - the FT8 / FT4 / FT2 window
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "app_window.h"
#include "ft8/decode_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opened from the Modes menu */
extern dialog_t *dialog_ft8;

/* The app as the browser panel shows it (remote_server.c). Any thread */
typedef struct {
    bool    open;           /* the app is on the screen */
    bool    cq, tx;         /* TX CQ, TX Call */
    bool    auto_on, hold;  /* Auto, Hold Freq */
    bool    sending;        /* on the air now */
    int     proto;          /* FTX_PROTOCOL_FT4 (0), FT8 (1), FT2 (2) */
    int32_t tx_freq;        /* audio Hz */
    char    next[40];       /* the next transmission, "" none */
} ft8_status_t;

void ft8_window_status(ft8_status_t *s);

/* What the panel asks: the buttons' own handlers. LVGL thread; false when the app is not open */
typedef enum { FT8_RC_CQ, FT8_RC_STOP, FT8_RC_AUTO, FT8_RC_HOLD, FT8_RC_MODE, FT8_RC_TXFREQ, FT8_RC_REPLY } ft8_rc_t;

bool ft8_window_remote(ft8_rc_t what, int32_t arg, const cell_data_t *cell);


#ifdef __cplusplus
}
#endif
