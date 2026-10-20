/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Where the CW decoder's text and speed go: the panel (and the remote
 *  clients), or the CW trainer while it runs.
 */

#pragma once

#include <stdbool.h>

#include "cw_decoder/morse_code.h"

/* Redirects the decoded text and the WPM estimate (called from the audio
 * thread) instead of the panel; NULL, NULL restores the panel. */
void cw_decoder_set_sink(void (*text)(const char *), void (*wpm)(int));

/* Decoder output (audio thread): to the sink if set, else to the panel */
void cw_decoder_text(const char *text);
void cw_decoder_wpm(int wpm);
