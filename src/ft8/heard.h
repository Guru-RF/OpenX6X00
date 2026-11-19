// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *  Xiegu X6100 LVGL GUI - the FT8/FT4/FT2 stations heard: the tiles and the calls over the waterfall
 *
 *  One entry a station, from the rows the list shows, with when it last called us; their order on the tiles, what a
 *  tile's tag says, Show: CQ over them, and the countries of the log that tell a NEW one. After F4OLI's redesign of
 *  the app (OlivLoze's fork). LVGL thread only; the times are lv_tick's milliseconds, passed in (a host test passes
 *  its own: tools/ft8_tiles).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "decode_list.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FT8_HEARD_MAX       64
#define FT8_HEARD_KEEP_MS   (2 * 60 * 1000)     /* a station goes 2 minutes after its last row... */
#define FT8_CALLS_YOU_MS    (3 * 60 * 1000)     /* ...one that calls us is red this long after its call, and stays */

typedef struct {
    cell_data_t cd;             /* its last row: a press calls it as a press on that row does */
    uint32_t    heard_ms;       /* when that row came */
    uint32_t    called_ms;      /* its last call to us (called) */
    bool        called;         /* it has called us since it was heard first */
    bool        logged;         /* our QSO with it is logged since its last call: no longer red */
    bool        has_report;     /* a call of it gave us a report: report */
    int         report;
} ft8_heard_t;

/* What a tile says, its colour the window's */
typedef enum {
    FT8_TAG_CALLS_YOU,      /* "calls you -12": a call to us in the last 3 minutes, the tile red */
    FT8_TAG_NEW,            /* its CQ, from a country not in the log */
    FT8_TAG_CQ,             /* "CQ" ("CQ POTA"): a CQ, not worked */
    FT8_TAG_WORKED,         /* a CQ, worked before (on this band and mode or another) */
    FT8_TAG_CALLED_YOU,     /* called us longer ago, in a QSO now or quiet */
    FT8_TAG_QSO,            /* in a QSO with another station */
} ft8_tag_t;

/* No station: another band or mode */
void ft8_heard_clear(void);

/* A row the list shows: a decode with its sender's call (a CQ, a message, one to us) adds or updates its station.
 * A call to us is any message to us but its RR73 or 73, which end the QSO */
void ft8_heard_add(const cell_data_t *cd, uint32_t now_ms);

/* The QSO with call is logged: its station's red ends (a new call to us brings it back) */
void ft8_heard_logged(const char *call);

/* Called us in the last FT8_CALLS_YOU_MS, and not logged since: the red tile, the call red over the waterfall */
bool ft8_heard_calls_you(const ft8_heard_t *st, uint32_t now_ms);

/* On the tiles and over the waterfall: everything with Show: All, the CQs and those calling us with Show: CQ, as the
 * list shows them (a station seen in a QSO under Show: All goes at once when Show turns to CQ) */
bool ft8_heard_shown(const ft8_heard_t *st, bool show_all, uint32_t now_ms);

/* The stations shown, at most max, in the tiles' order: those calling us (the newest call first), then the CQs of a
 * country not in the log, the CQs not worked, the CQs worked, the others; the strongest first in each. Those not
 * heard for FT8_HEARD_KEEP_MS go first (one calling us stays while it is red). Pointers into the list, valid until
 * the next add or clear */
int ft8_heard_list(const ft8_heard_t **out, int max, bool show_all, uint32_t now_ms);

ft8_tag_t ft8_heard_tag(const ft8_heard_t *st, uint32_t now_ms);

/* The countries of the log (cty.dat's primary prefixes): a decode from another one is NEW */
void ft8_worked_clear(void);
void ft8_worked_add(const char *prefix);
bool ft8_worked_has(const char *prefix);

#ifdef __cplusplus
}
#endif
