/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax Bulletin: each kind of chart of the plan (wefax/plan.h) as a tile with its newest copy, how
 *  old it is, its mark and when the next one comes; the MFK picks one, a press shows it large with its details, a
 *  second press over the whole screen (wefax_viewer.h). The chart coming in draws in its tile. From OlivLoze's fork
 *  (F4OLI), for a radio that keeps the charts off-grid.
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The chart Auto Plan is taking now, from the WeFax app */
typedef struct {
    const char *product;                    /* NULL: none */
    int         permille;                   /* of its slot's length */
    unsigned    rows;                       /* its rows so far, WEFAX_WIDTH wide */
    const uint8_t *(*row)(unsigned i);      /* 0 = the oldest */
} wefax_bulletin_live_t;

typedef struct {
    void (*live)(wefax_bulletin_live_t *out);
    void (*open_file)(const char *name);    /* the fax browser on this file */
    lv_event_cb_t other_keys;               /* the keys the Bulletin leaves (VOL) */
    void (*changed)(void);                  /* tiles <-> large: the buttons' labels */
    void (*view_file)(const char *name, const char *title);   /* the large one over the whole screen */
} wefax_bulletin_cbs_t;

/* Over the whole of parent (the app's dialog), the keys taken */
void wefax_bulletin_open(lv_obj_t *parent, const wefax_bulletin_cbs_t *cbs);
void wefax_bulletin_close(void);
bool wefax_bulletin_is_open(void);

/* The big view of one kind (else the tiles) */
bool wefax_bulletin_one(void);
/* The MFK's press: the tile large, the large one over the whole screen */
void wefax_bulletin_enter(void);
/* Fix Chart: the large one in the fax browser, for Fix Align and Fix Tilt */
void wefax_bulletin_fix(void);
/* The keys back to the Bulletin (after the full screen viewer) */
void wefax_bulletin_focus(void);
/* Back from the large view to the tiles */
void wefax_bulletin_back(void);

#ifdef __cplusplus
}
#endif
