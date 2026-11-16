/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  HF Air Band: the HF aeronautical frequencies (aero.h) to choose and tune
 */

#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_aero;

/* The area the app shows (aero_area_t): the one chosen with Area, else the radio's region's. The Scanner's HF Air list
 * is the same */
int dialog_aero_area(void);

#ifdef __cplusplus
}
#endif
