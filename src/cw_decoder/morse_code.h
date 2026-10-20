/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Morse code table, shared by the CW keyer (text -> code) and the
 *  decoder (code -> text).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    *morse;         /* dots and dashes, e.g. ".-" */
    char    *character;     /* "A", or a prosign as "<AR>" */
} cw_characters_t;

/* Ends with a NULL morse. The keyer takes the first entry whose text matches. */
extern cw_characters_t cw_characters[];

/* Text for a code, NULL if there is none */
const char *morse_to_text(const char *code);

#ifdef __cplusplus
}
#endif
