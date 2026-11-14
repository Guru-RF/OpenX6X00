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
    char    *character;     /* "A", a letter of two bytes in UTF-8 ("\xc3\x85", Å), "CH", or a prosign as "<AR>" */
} cw_characters_t;

/* Ends with a NULL morse. The keyer takes the first entry whose text matches (morse_find). */
extern cw_characters_t cw_characters[];

/* The entry the keyer takes for the start of text: the first whose text it starts with, small letters as the
 * capitals (Latin-1's too: "\xc3\xa5", å, is Å). The bytes it takes are strlen(->character). NULL if none */
const cw_characters_t *morse_find(const char *text);

/* Text for a code, NULL if there is none */
const char *morse_to_text(const char *code);

#ifdef __cplusplus
}
#endif
