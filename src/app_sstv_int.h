/*
 * SSTV app: what its three parts share.
 *   app_sstv.c          window, buttons, receiving (Auto and Manual RX)
 *   app_sstv_tx.c       TX menus, the picture that goes out, transmission
 *   app_sstv_gallery.c  saved pictures
 * Everything here runs in the LVGL thread unless said otherwise.
 */
#pragma once

#include "app_frame.h"
#include "board.h"
#include "app_sstv.h"
#include "buttons.h"
#include "dialog.h"
#include "sstv/sstv_mode.h"

#include <stdbool.h>
#include <stdint.h>

/* The picture area: the body height, 4:3, at the left of the body. VISION: the window's whole
 * height, the header and the state in a column at its right (SSTV_COL_X) */
#define SSTV_PIC_H      (theme_shape.vision ? DIALOG_H - 20 : APP_FRAME_BODY_H - 2)
#define SSTV_PIC_W      (SSTV_PIC_H * 4 / 3)
#define SSTV_PIC_X      (APP_FRAME_X + 1)
#define SSTV_PIC_Y      (theme_shape.vision ? 10 : APP_FRAME_BODY_Y + 1)
#define SSTV_COL_X      (SSTV_PIC_X + SSTV_PIC_W + 15)
#define SSTV_COL_W      (APP_FRAME_X + APP_FRAME_W - 8 - SSTV_COL_X)
#define SSTV_PIC_PIXELS (((DIALOG_H_MAX - 20) * 4 / 3) * (DIALOG_H_MAX - 20))   /* largest dialog */

#define SSTV_DIR        BOARD_DATA "/sstv"
#define SSTV_HELP_LINE  "Freq.: calling frequencies   Pictures in /mnt/sstv"

/* ---- app_sstv.c ---- */
extern app_frame_t  sstv_frame;
extern lv_obj_t    *sstv_canvas;          /* the picture area, NULL while closed */
extern lv_obj_t    *sstv_text;            /* state text right of the picture     */
extern lv_obj_t    *sstv_bar;             /* progress bar of the app look        */
extern lv_color_t   sstv_pixels[SSTV_PIC_PIXELS];   /* sstv_canvas buffer, row SSTV_PIC_W */
extern button_data_t sstv_btn_mode, sstv_btn_tx, sstv_btn_gallery, sstv_btn_delete;

/* The RX/TX Mode button: the mode of Manual RX and of TX; sstv_mode_auto:
 * Manual RX finds the mode by itself */
extern sstv_mode_t  sstv_mode;
extern bool         sstv_mode_auto;

void sstv_status(const char *text);       /* text right of the picture, its first line in the header */
void sstv_controls(void);                 /* enable the RX buttons that make sense now */
void sstv_mode_button(bool tx, sstv_mode_t tx_mode);   /* "RX Mode" / "TX Mode <tx_mode>" label */
void sstv_focus_picture(void);            /* MFK and volume keys back to the picture */
void sstv_key_cb(lv_event_t *e);          /* volume on the knob while a list has the focus */

/* Receivers stopped for TX setup; true if Auto RX was on */
bool sstv_rx_stop_for_tx(void);
/* Back from TX: Auto RX listening, or stand-by */
void sstv_rx_back(bool auto_rx);

/* ---- app_sstv_tx.c ---- */
typedef enum { SSTV_TX_OFF = 0, SSTV_TX_SETUP, SSTV_TX_ON_AIR } sstv_tx_state_t;

extern sstv_tx_state_t sstv_tx_state;

void        sstv_tx_button(void);         /* TX / Cancel TX / Halt TX */
void        sstv_tx_halt(void);           /* any thread */
void        sstv_tx_mode_changed(void);   /* TX Mode pressed during setup */
void        sstv_tx_tick(void);           /* from the UI timer */
void        sstv_tx_close(void);          /* window closing: waits for the TX thread */
bool        sstv_tx_busy(void);
int         sstv_tx_progress(void);       /* 0..1000 */
sstv_mode_t sstv_tx_mode(void);           /* mode on the air */
/* Web remote: picture `file` of the TX folder, as it is, in mode */

/* ---- app_sstv_gallery.c ---- */
void sstv_gallery_button(void);           /* RX Pics Browser / Close Browser */
void sstv_gallery_second(void);           /* Delete picture / Done */
void sstv_gallery_close(void);
bool sstv_gallery_open(void);
void sstv_gallery_labels(void);
