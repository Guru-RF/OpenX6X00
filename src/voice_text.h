/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - decoded text read aloud (accessibility): what the CW decoder, RTTY, PSK, Olivia and JTTY receive
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Received text as a decoder gives it, a character or a few at a time, any thread. Said in bursts: at a line break,
 * after a pause, or once a burst is long, and only once the announcement before it has ended. Settings > Voice >
 * Read decoded text turns it off */
void voice_text_add(const char *text);

/* A message received whole (JTTY): said once, when complete, any thread */
void voice_text_message(uint32_t id, const char *text, bool complete);

#ifdef __cplusplus
}
#endif
