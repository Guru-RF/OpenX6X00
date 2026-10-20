/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: parks (POTA), summits (SOTA), bunkers (WWBOTA) and HOTA sites near a position,
 *  from the list x6100-field update writes (one reference per line, tab separated:
 *  type P/S/B/H, ref, name, lat, lon, alt m, points).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  type;         /* 'P' park, 'S' summit, 'B' bunker, 'H' HOTA site */
    char  ref[16];
    char  name[48];
    double lat, lon;
    int   alt_m;        /* summits */
    int   points;       /* summits */
    float km;
} field_ref_t;

/* "S\tI/LO-123\tName\t45.1\t9.2\t1200\t4" -> r (the line is cut); false when malformed */
bool field_ref_parse(char *line, field_ref_t *r);

/* The max nearest references of the type ('P', 'S', 'B', 'H'), nearest first; returns the count,
 * -1 when the file can't be read */
int field_refs_near(const char *path, char type, double lat, double lon, field_ref_t *out, int max);

/* The reference, by name (case-insensitive) */
bool field_refs_find(const char *path, const char *ref, field_ref_t *out);

#ifdef __cplusplus
}
#endif
