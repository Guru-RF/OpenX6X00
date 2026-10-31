/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  The five buttons over F1-F5 and their pages. A page is five button_data_t; the
 *  apps make their own pages and load them, the front keys load the groups below.
 */

#pragma once

#include "mfk_control.h"
#include "vol_control.h"
#include "cfg/cfg_api.h"
#include "gui_styles.h"
#include "theme.h"

#define BUTTONS     5
#define BTN_HEIGHT  BUTTONS_H
#define BTN_WIDTH   160
#define BTN_INSET   (theme_kx3 ? 0 : 1 + theme_shape.btn_gap)   /* space on every side of a drawn button; KX3Lovers Plus: none */

#ifdef __cplusplus
#include "params/params.h"
extern "C" {
#endif

#include "lvgl/lvgl.h"
#include "screen_main.h"

typedef enum {
    BTN_EMPTY,
    BTN_TEXT,       /* label: fixed text */
    BTN_TEXT_FN,    /* label_fn: text made at each show, again when subj changes */
} btn_type_t;

typedef struct _disp_button_t disp_btn_t;

/* "Name:\nvalue" texts show a small name over the value, "(PAGE 1:2)" a page button */
typedef struct button_data_t {
    btn_type_t type;
    union {
        const char *label;
        const char *(*label_fn)();
    };
    void (*press)(struct button_data_t *);
    void (*hold)(struct button_data_t *);
    struct buttons_page_t *next;        /* page buttons: the pages F1 and held F1 go to */
    struct buttons_page_t *prev;
    int32_t                ctrl;        /* knob control, app action or memory, by the press */
    bool                   encoder_allowed;  /* ctrl is a knob control: show its knob mark */
    disp_btn_t            *disp_btn;    /* where it is shown now, or NULL */
    Subject               *subj;
    Observer              *observer;
    bool                   mark;
    bool                   disabled;
} button_data_t;

typedef struct buttons_page_t {
    button_data_t *items[BUTTONS];
} buttons_page_t;

typedef buttons_page_t *buttons_group_t[];

extern buttons_page_t buttons_page_vol_1;
extern buttons_page_t buttons_page_rtty;
extern button_data_t  btn_dig_pwr;   /* TX power steps for the digital apps */
extern button_data_t  btn_ext_tune;  /* TUNE for an external tuner, on and off (ext_tune.h) */
extern button_data_t  btn_tx_pwr;    /* TX power on the knob (walkie.c too) */

/* GEN APP KEY DFN DFL VM: the key again shows the next page */
extern buttons_group_t buttons_group_gen;
extern buttons_group_t buttons_group_app;
extern buttons_group_t buttons_group_key;
extern buttons_group_t buttons_group_dfn;
extern buttons_group_t buttons_group_dfl;
extern buttons_group_t buttons_group_vm;

void            buttons_init(lv_obj_t *parent);
/* The five buttons to parent in a column, w x h each, F1 on top (the walkie screen) */
void            buttons_vertical(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);
/* Back in a row at the bottom of parent, where they were (the main screen) */
void            buttons_horizontal(lv_obj_t *parent);
/* Show the button again (label, mark, disabled) if it is on screen */
void            buttons_refresh(button_data_t *data);
void            buttons_refresh_marks();
void            buttons_screen_rec_update();
void            buttons_mark(button_data_t *data, bool val);
void            buttons_disabled(button_data_t *data, bool val);
void            buttons_load(uint8_t n, button_data_t *data);
void            buttons_load_page(buttons_page_t *page);
void            buttons_unload_page();
/* press and hold of a page button */
void            button_next_page_cb(button_data_t *data);
void            button_prev_page_cb(button_data_t *data);
void            buttons_press(uint8_t n, bool hold);
void            buttons_load_page_group(buttons_group_t group);
buttons_page_t *buttons_get_cur_page();

#ifdef __cplusplus
}
#endif
