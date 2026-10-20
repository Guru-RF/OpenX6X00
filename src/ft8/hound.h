/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  FT8 DXpedition mode, Hound side: the sequencing rules of WSJT-X
 *  (widgets/mainwindow.cpp, GPL v3, K1JT and the WSJT-X team)
 */

#pragma once

#include <stdbool.h>
#include <time.h>

#define HOUND_MIN_TX_HZ  1000   /* the Fox works below 1000 Hz: the Hounds call above */
#define HOUND_CALL_S     120    /* calling stops after 2 minutes */
#define HOUND_REPORT_S   165    /* R+report stops after 11 FT8 periods */

typedef enum {
    HOUND_IDLE,
    HOUND_CALLING,   /* "FOX MYCALL GRID" */
    HOUND_REPORT,    /* "FOX MYCALL R-12", after the Fox's report */
} hound_stage_t;

typedef enum {
    HOUND_NONE,
    HOUND_REPLY,     /* the Fox sent us a report: tx holds the R+report */
    HOUND_LOGGED,    /* the Fox sent RR73: log the QSO, stop sending */
} hound_event_t;

typedef struct {
    hound_stage_t stage;
    char          my_call[14];
    char          fox[14];
    char          fox_grid[9];
    int           fox_freq;     /* the Fox's audio frequency when it sent the report */
    bool          have_report;
    int           rcvd;         /* report from the Fox */
    int           sent;         /* our report of the Fox */
    int           r_tx;         /* R+report transmissions since the last report */
    time_t        since;        /* start of this stage */
} hound_t;

/* Start calling the Fox: tx gets "FOX MYCALL GRID4" (35 bytes) */
void          hound_call(hound_t *h, const char *my_call, const char *my_grid, const char *fox, const char *fox_grid,
                         time_t now, char *tx);
/* A decode; odd: received in the second period (other Hounds, ignored) */
hound_event_t hound_rx(hound_t *h, const char *text, int snr, int freq_hz, bool odd, time_t now, char *tx);
/* The audio frequency for the next transmission (counts the R+report ones); rnd: any random number */
int           hound_tx_freq(hound_t *h, int cur_hz, unsigned rnd);
/* Calling or R+report went on too long: back to idle */
bool          hound_timed_out(hound_t *h, time_t now);
void          hound_stop(hound_t *h);

/* The Fox's multi-stream message (i3=0 n3=1) as text: false for any other kind */
#include <ft8lib/message.h>
bool          hound_unpack_dxpedition(const ftx_message_t *msg, char *text);
