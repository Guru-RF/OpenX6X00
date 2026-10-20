/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW text parser: splits run-together decoded words on the usual CW tokens and callsigns
 *  ("CQCQDEON6UREK" -> "CQ CQ DE ON6URE K") and tells calls, abbreviations and RST apart.
 *  It only ever inserts spaces. Plain C, no heap, no LVGL (tests/test_cw_parse.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CWP_PLAIN,
    CWP_ABBR,       /* CQ DE K TU 73 ..., prosigns <AR> */
    CWP_RST,        /* 599 5NN 579 */
    CWP_CALL,       /* callsign shape */
} cwp_class_t;

/* One word (no spaces) of n chars into out, spaces where it splits; returns the out length.
 * Unchanged when it does not split cleanly. text (or NULL): what came before; a call already
 * there decides a tie ("W1AWK" or "W1AW K"). out needs 2n+1 bytes */
size_t cw_parse_split(const char *w, size_t n, const char *text, char *out, size_t size);

cwp_class_t cw_parse_class(const char *w, size_t n);
bool        cw_parse_is_call(const char *w, size_t n);

/* w, a word inside text (words split by ' ' and '\n'), is a call sent as one: after or before DE,
 * at least twice in the text, or known */
bool cw_parse_confirmed(const char *text, const char *w, size_t n);

/* Calls confirmed from outside (a logbook lookup), the last 32: they count as confirmed
 * and win splits. know: false when not a call or already known. Not thread safe: one thread */
bool cw_parse_know(const char *call, size_t n);
bool cw_parse_known(const char *w, size_t n);
void cw_parse_forget(void);
/* The i-th known call, NULL past the last */
const char *cw_parse_known_at(size_t i);

#ifdef __cplusplus
}
#endif
