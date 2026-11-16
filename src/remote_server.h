/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Browser remote: the screen streamed as JPEG frames over a WebSocket, the front
 *  keys and the three knobs driven back (remote_server.c). The web configurator's Remote page
 *  is its client; audio goes over the TCI server (tci_server.c).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "lvgl/lvgl.h"
#include "board.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_CONF         BOARD_DATA "/remote.conf"
#define REMOTE_SESSIONS     BOARD_DATA "/remote_sessions.conf"   /* the paired browsers (R3) */
#define REMOTE_DEFAULT_PORT 50002

void remote_server_init(void);

/* On/off (saved in REMOTE_CONF, the web configurator reads and writes it too) and the port */
bool remote_server_get_enabled(void);
void remote_server_set_enabled(bool on);
int  remote_server_port(void);

/* Viewers connected now, and the address of one of them (NULL without) */
int         remote_server_viewers(void);
const char *remote_server_viewer(void);

/* Sessions (R3): the pairing code, shown on the screen for two minutes and copied to buf; the
 * paired browsers as "name (admin) *, ..." (* connected) and their count; every session
 * forgotten; every socket closed (the sessions stay) */
void remote_server_pair_code(char *buf, size_t n);
int  remote_server_sessions(char *buf, size_t n);
void remote_server_forget_all(void);
void remote_server_disconnect_all(void);

/* Every area LVGL flushes to the panel (screen_rec.c calls it): kept for the viewers */
void remote_flush(const lv_area_t *area, const lv_color_t *color_p);

/* For the Panel page: a row of the FT8 app's list (a cell_data_t, LVGL thread) and the CW
 * decoder's text (any thread) */
void remote_server_ft8_row(const void *cell);
void remote_server_cw_text(const char *text);

/* For the WeFax page: a row of the picture coming in (LVGL thread), to the pages that asked ("wefax rows 1");
 * number counts the picture's rows from 1, a new picture starts at 1 again */
void remote_server_wefax_row(const uint8_t *row, unsigned width, uint32_t number);

/* For the FT8 and JS8 pages: a row of the open app's own waterfall in dB over the noise floor, low to high Hz
 * (LVGL thread), to the pages that asked ("ft8 wf 1", "js8 wf 1") */
void remote_server_app_wf(const float *db, unsigned n, int lo_hz, int hi_hz);

/* For the JS8 page. The JS8 app (js8_window.c) hands its messages and its state over in these types of the
 * protocol's own, and takes the page's commands through the two functions at the end, which it implements */
#define REMOTE_JS8_TEXT 640                 /* a message's text with its NUL, as the receiver's (JS8_RX_TEXT_LEN) */

typedef struct {
    uint32_t msg_id;                        /* the receiver's: the same while a message comes in frame by frame */
    int32_t  utc;                           /* hhmmss */
    int      snr, freq_hz, submode;         /* submode: 0 Normal, 1 Fast, 2 Turbo, 4 Slow */
    bool     tx;                            /* ours */
    bool     to_me, cq, heartbeat, to_group, low_confidence, partial;
    int      checksum;                      /* a buffered command's (MSG ...): 1 good, -1 wrong, 0 none */
    char     from[16], to[16];              /* "" unknown */
    char     grid[8];                       /* the sender's, when the app knows it */
    char     text[REMOTE_JS8_TEXT];
} remote_js8_msg_t;

/* LVGL thread: kept for a page that opens later, and to the pages now ("js8r {...}"). A message still coming in
 * keeps its place and its id as it grows */
void remote_server_js8_msg(const remote_js8_msg_t *m);

typedef struct {
    bool     open;                          /* the app is on the screen */
    bool     busy, sending;                 /* a message queued or on the air; keying now */
    bool     auto_cq, hb, auto_on, hold;
    int      speed;                         /* 0 Normal, 1 Fast, 2 Turbo, 3 Slow */
    int      frame, frames;                 /* of the message on the air */
    int32_t  txf;                           /* our TX offset, Hz */
    int32_t  sel_hz;                        /* the selected station's offset, -1 none */
    int      hb_min;                        /* the heartbeat interval, minutes */
    int32_t  next_s;                        /* seconds to the next frame, heartbeat or CQ, -1 none */
    uint32_t notes;                         /* counts the answers to the pages' commands */
    bool     note_err;                      /* the last answer is a refusal */
    int      note_cmd;                      /* the remote_js8_rc_t it answers */
    char     note[80];                      /* "Queued: 3 frames, 45 s", "Already sending: Stop first" */
    char     sel[16];                       /* the selected station, "" none */
    char     where[40];                     /* "JS8 7078.0 kHz", or the preset's name */
    char     text[160];                     /* the message queued or on the air, "" none */
} remote_js8_status_t;

typedef enum {
    REMOTE_JS8_CQ,                          /* one CQ now, or auto CQ off */
    REMOTE_JS8_AUTOCQ,                      /* auto CQ on: a CQ now, then one each interval */
    REMOTE_JS8_HBNOW,                       /* one heartbeat now */
    REMOTE_JS8_HB,                          /* heartbeats every interval, on or off */
    REMOTE_JS8_AUTO, REMOTE_JS8_HOLD, REMOTE_JS8_SPEED, REMOTE_JS8_STOP,
    REMOTE_JS8_TXFREQ,                      /* arg: Hz */
    REMOTE_JS8_SELECT,                      /* text: a call heard on this band */
    REMOTE_JS8_SEND,                        /* text: the message, its macros filled as on the radio */
} remote_js8_rc_t;

/* In js8_window.c: the app's state (any thread), whether it is open (LVGL thread), and a command of the page's (LVGL
 * thread): false when the app is closed or refuses it, and the status' note says why */
void js8_window_status(remote_js8_status_t *s);
bool js8_window_running(void);
bool js8_window_remote(remote_js8_rc_t what, int32_t arg, const char *text);

#ifdef __cplusplus
}
#endif
