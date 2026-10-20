/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - NavTex app: NAVTEX (SITOR-B) text and DSC calls, receive only
 */
#ifndef DIALOG_NAVTEX_H
#define DIALOG_NAVTEX_H
#include "app_window.h"     /* dialog_t */
#include <stdbool.h>    /* bool */
#include <stddef.h>
#include <stdint.h>
#if defined(__cplusplus)
extern "C" {
#endif /* __cplusplus */
extern dialog_t *dialog_navtex;         /* the app, opened from the APP menu */
bool dialog_navtex_is_active(void);     /* the app is on screen */
#if defined(__cplusplus)
}   /* extern "C" */
#endif /* __cplusplus */
#endif /* DIALOG_NAVTEX_H */
