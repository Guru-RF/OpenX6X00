// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *  Xiegu X6100 LVGL GUI - the FT8/FT4/FT2 message list
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  The list of decodes, sent messages and slot rows of the FT8 window, with the "For you"
 *  table under it. LVGL thread only: the decoder thread hands its rows over with
 *  scheduler_put(table_view_add_msg_cb, &cell, sizeof(cell)).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "../qso_log.h"
#include "qso.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a row is: its colours and whether it can be answered */
typedef enum {
    CELL_RX_INFO = 0,   /* "RX FT8 20m 12:00:15" and the app's notes */
    CELL_START_QSO,     /* "Start QSO with ..." */
    CELL_RX_MSG,
    CELL_RX_CQ,
    CELL_RX_TO_ME,
    CELL_TX_MSG,
} ft8_cell_type_t;

/* One row: its text and, for a decode, what a press on it needs */
typedef struct {
    ft8_cell_type_t         cell_type;
    int16_t                 dist;           /* km to the sender's grid, 0 unknown */
    bool                    odd;            /* received in an odd slot */
    ftx_msg_meta_t          meta;
    char                    text[64];       /* what the row shows */
    qso_log_search_worked_t worked_type;    /* CQ rows: worked before */
    char                    country[24];    /* the sender's country, "" unknown (dxcc.c) */
    char                    cty_prefix[8];  /* its primary prefix ("DL") */
    int16_t                 flag;           /* its flag in flags.h, -1 none */
    bool                    pota;           /* spotted on POTA now, or a CQ POTA */
} cell_data_t;

/* What the window does on keys the list gets */
typedef struct {
    void (*on_close)(void);             /* ESC */
    void (*on_vol_change)(int32_t dir); /* VOL turned: -1 / +1 */
} table_view_actions_t;

/* A press on the list: the row's data, NULL on the "Wait sync" row */
typedef void (*table_view_press_cb_t)(const cell_data_t *cell);

/* The list at x, y, w x h on parent (the pin table takes its bottom when it has rows) */
void      table_view_build(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
void      table_view_destroy(void);
/* The "For you" rows go */
void      table_view_pin_clear(void);
/* The list's LVGL table, for focus and fading */
lv_obj_t *table_view_obj(void);

void table_view_set_press_cb(table_view_press_cb_t cb);
void table_view_set_actions(const table_view_actions_t *a);

/* Empty again: the "Wait sync" row alone, no "For you" rows */
void table_view_reset(void);

/* A row at the bottom (a copy is kept); a slot row after a slot row only updates it */
void table_view_push_ui(const cell_data_t *cell);

/* table_view_push_ui() for scheduler_put(): data is a cell_data_t */
void table_view_add_msg_cb(void *data);

/* Flag k of flags.h (flag_of[].flag) as an LVGL image, NULL for none */
const lv_img_dsc_t *table_view_flag(int k);

#ifdef __cplusplus
}
#endif
