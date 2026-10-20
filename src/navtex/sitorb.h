/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - NAVTEX receiver: SITOR-B (CCIR 476 FEC), 100 Bd FSK,
 *  mark 1085 Hz / space 915 Hz in mono audio at 11025 Hz (the WeFax stream).
 *
 *  Plain SITOR-B: every character is handed out as soon as it is read, no message
 *  assembly (ZCZC / NNNN stay in the text). All callbacks run in the thread that
 *  calls navtex_decoder_process().
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct navtex_decoder navtex_decoder_t;
typedef void (*navtex_char_cb_t)(int c, void *userdata);

/* reverse != 0 swaps mark and space; NULL when out of memory */
navtex_decoder_t *navtex_decoder_create(int reverse);
void navtex_decoder_destroy(navtex_decoder_t *decoder);

/* Decoded characters: ASCII, '\n' for line feed (carriage returns are dropped) */
void navtex_decoder_set_char_callback(navtex_decoder_t *decoder, navtex_char_cb_t cb, void *userdata);

/* Every demodulated bit, mark = 1 for the higher tone (the DSC receiver, dsc.h) */
void navtex_decoder_set_bit_callback(navtex_decoder_t *decoder, void (*cb)(int mark, void *userdata), void *userdata);

void navtex_decoder_process(navtex_decoder_t *decoder, const float *samples, unsigned int n);

#ifdef __cplusplus
}
#endif
