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
    /* What the automation will do next (the panel's timeline, U10) */
    int32_t in_ms;          /* ms until the start of the slot the next transmission goes in, -1 none */
    int32_t slot_ms;        /* the slot's length */
    int     repeats;        /* times the queued message still goes out without an answer, -1 no limit */
    bool    qso;            /* a QSO is under way (a station answered, or was answered) */
    bool    hound;          /* Hound mode */
    bool    nocall;         /* no callsign on the radio: nothing transmits */
    char    logged[16];     /* the last QSO saved since the app opened, "" none */
    int32_t logged_s;       /* seconds since, -1 none */
} ft8_status_t;

void ft8_window_status(ft8_status_t *s);

/* What the panel asks: the buttons' own handlers. LVGL thread; false when the app is not open */
typedef enum { FT8_RC_CQ, FT8_RC_STOP, FT8_RC_AUTO, FT8_RC_HOLD, FT8_RC_MODE, FT8_RC_TXFREQ, FT8_RC_REPLY, FT8_RC_TXCALL } ft8_rc_t;

bool ft8_window_remote(ft8_rc_t what, int32_t arg, const cell_data_t *cell);


#ifdef __cplusplus
}
#endif
