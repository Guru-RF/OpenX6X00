/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: parks (POTA), summits (SOTA), bunkers (WWBOTA), HOTA sites, WWFF areas,
 *  towers (TOTA), castles (WCA) and mills (MOTA) near a position, from the list x6100-field update writes (one
 *  reference per line, tab separated: type P/S/B/H/F/T/C/M, ref, name, lat, lon, alt m, points; lat and lon
 *  empty for a castle the list has no position for).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  type;         /* 'P' park, 'S' summit, 'B' bunker, 'H' HOTA site, 'F' WWFF area, 'T' tower, 'C' castle, 'M' mill,
                         * 'L' lighthouse; 'G' a Tiles tile, worked out from the position (no list) */
    char  ref[16];
    char  name[48];
    double lat, lon;    /* NAN when the list has no position for it */
    int   alt_m;        /* summits */
    int   points;       /* summits */
    float km;
} field_ref_t;

/* "S\tI/LO-123\tName\t45.1\t9.2\t1200\t4" -> r (the line is cut); false when malformed */
bool field_ref_parse(char *line, field_ref_t *r);

/* The max nearest references of the type ('P', 'S', 'B', 'H'...), nearest first, those with no position left out;
 * returns the count, -1 when the file can't be read */
int field_refs_near(const char *path, char type, double lat, double lon, field_ref_t *out, int max);

/* The reference, by name (case-insensitive) */
bool field_refs_find(const char *path, const char *ref, field_ref_t *out);

#ifdef __cplusplus
}
#endif
