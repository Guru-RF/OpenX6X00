/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax charts as tiles, two views of the saved charts:
 *    - the Bulletin: each kind of chart of the plan (wefax/plan.h) with its newest copy of fair quality, how old it
 *      is, its quality and when the next one comes; the chart coming in draws in its tile;
 *    - All Charts: every chart saved, newest first, named in words (Analysis, Forecast 48 h, Sea and swell...).
 *  The MFK picks a tile, a press shows it large (the whole chart, its station and frequency, when it was received,
 *  its quality, the next copy; for a kind with none, its coming broadcasts), a second press over the whole screen
 *  (wefax_viewer.h). Delete takes a second press within 3 s. From OlivLoze's fork (F4OLI), for a radio that keeps
 *  the charts off-grid.
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEFAX_TILES_BULLETIN = 0,   /* a tile a kind of chart */
    WEFAX_TILES_ALL,            /* a tile a chart, newest first */
} wefax_tiles_mode_t;

/* The chart Auto Plan is taking now, from the WeFax app */
typedef struct {
    const char *product;                    /* NULL: none */
    int         permille;                   /* of its slot's length */
    unsigned    rows;                       /* its rows so far, WEFAX_WIDTH wide */
    const uint8_t *(*row)(unsigned i);      /* 0 = the oldest */
} wefax_bulletin_live_t;

typedef struct {
    void (*live)(wefax_bulletin_live_t *out);
    lv_event_cb_t other_keys;               /* the keys the tiles leave (VOL) */
    void (*changed)(void);                  /* tiles <-> large, Delete asked: the buttons */
    void (*view_file)(const char *name, const char *title, bool fix);   /* over the whole screen; fix: to correct */
    void (*back)(void);                     /* ESC on the tiles: as F5, back to the fax */
} wefax_bulletin_cbs_t;

/* Over the whole of parent (the app's dialog), the keys taken */
void wefax_bulletin_open(lv_obj_t *parent, wefax_tiles_mode_t mode, const wefax_bulletin_cbs_t *cbs);
void wefax_bulletin_close(void);
bool wefax_bulletin_is_open(void);
wefax_tiles_mode_t wefax_bulletin_mode(void);

/* The large view of one (else the tiles) */
bool wefax_bulletin_one(void);
/* The one chosen has a chart saved (a kind of the Bulletin may have none yet) */
bool wefax_bulletin_has_chart(void);
/* The MFK's press: the tile large, the large one over the whole screen */
void wefax_bulletin_enter(void);
/* Fix: the one chosen over the whole screen, straight into its Tilt and Align */
void wefax_bulletin_fix(void);
/* Back from the large view to the tiles; false when on the tiles already */
bool wefax_bulletin_back(void);
/* All Charts: the newest tile */
void wefax_bulletin_newest(void);
/* Delete the chart chosen: a first press asks for 3 s, a second one deletes. Asked now? */
void wefax_bulletin_delete(void);
bool wefax_bulletin_delete_armed(void);
/* A chart of the plan in words, translated: "Analysis", "Forecast 48 h"...; the name itself when it has none */
void wefax_bulletin_product_name(const char *product, char *out, size_t n);
/* The keys back to the tiles (after the full screen viewer), a chart saved there shown again */
void wefax_bulletin_focus(void);

#ifdef __cplusplus
}
#endif
