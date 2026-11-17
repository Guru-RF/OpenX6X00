/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax: a quality mark (0-100) of a received fax, so that Auto Plan keeps the best copy of each chart.
 *  The v2 scale of the mark F4OLI's own receiving station gives its charts (his qualite.py of 2 Nov 2026, ported by
 *  him in his fork of OpenX6X00: the same marks to 0.1 on the weekend's charts).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned lines;
    float    coherence;     /* a dark trace goes on into the next row, noise does not */
    float    fading;        /* rows whose mean is off the local norm */
    float    slip;          /* rows shifted > 3 px from the previous one (lost sync) */
    float    isolated;      /* dark pixels with no dark neighbour (crackle) */
    float    clean;         /* clean white 16 x 16 blocks */
    float    white;         /* white level: median of the rows' 90th percentile */
    float    contrast;      /* white minus the median of the dark pixels */
    float    ink;           /* dark pixels (< 110) */
    float    stripes;       /* rows whose background jumps > 4 levels (horizontal stripes) */
} wefax_quality_measures_t;

/* The mark of a fax, rows of width pixels (grey, 0 = black). no_stop: the stop tone was not heard (the footer is
 * probably cut), x 0.8. Slow (~0.5 s on the radio): a worker thread. m may be NULL */
float wefax_quality(const uint8_t *img, unsigned width, unsigned rows, bool no_stop, wefax_quality_measures_t *m);

/* A quick look at a few rows (> 20): the median correlation of the dark pixels of a row with the next one (a chart
 * 0.5-0.9, noise < 0.3) and in *dark the share of dark pixels (a chart mostly white, noise about half) */
float wefax_quality_rows(const uint8_t *img, unsigned width, unsigned rows, float *dark);

#ifdef __cplusplus
}
#endif
