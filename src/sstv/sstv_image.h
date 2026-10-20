/*
 * SSTV picture decoding (SSTV_RATE mono float audio).
 *
 * rgb buffers hold at least SSTV_MAX_W * SSTV_MAX_H * 3 bytes; a picture of
 * mode m fills width x height of sstv_mode_info(m), rows not received are
 * black.  The decoders return the number of rows decoded, or < 0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sstv_mode.h"
#include "sstv_sync.h"

/* An Auto RX capture: VIS found, picture after it (it is not looked for
 * again).  The first line sync is expected about 2 s into x. */
int sstv_decode(const float *x, long n, sstv_mode_t mode, uint8_t *rgb);

/* A Manual RX capture of mode: it may begin and end anywhere, no VIS */
int sstv_decode_manual(const float *x, long n, sstv_mode_t mode, uint8_t *rgb);

/* Instant frequency (Hz) of every sample of x[0..n), malloc'd (free() it).
 * About 10 samples at each end are not settled. */
float *sstv_fm(const float *x, long n);

/* Seconds from the end of the VIS to the sync a row hangs on: the line sync
 * (Robot, Martin), the sync between B and R (Scottie).  < 0: unknown mode */
double sstv_row0_after_vis(sstv_mode_t mode);

/* ---- Rows drawn while the picture arrives ----
 * Positions are sample indexes of the capture buffer.  Before the follower
 * locks, row 0 comes from the VIS and the nominal period; then from the
 * syncs it sees (so slant is followed).  The saved picture comes from
 * sstv_decode() at the end. */

/* Measured at 11025 Hz (tests/test_sstv.c): the VIS end lies this many
 * samples before the row-0 sync, the follower's sync start this many
 * samples before the true one */
#define SSTV_ROWS_VIS_BIAS  35.0
#define SSTV_ROWS_SYNC_BIAS 6.0

typedef struct {
    sstv_mode_t mode;
    int         width, height;
    double      row0;           /* row 0 sync, bias corrected; < 0 not known yet */
    double      nominal;        /* line, samples                                 */
    int         next;           /* first row not drawn                           */
    double      at, scale;      /* sync and scale of row next (from sstv_rows_ready) */
    long        from, to;       /* its audio                                     */
} sstv_rows_t;

/* row0_sync: VIS end + sstv_row0_after_vis() in samples, no bias; < 0 when
 * there is no VIS (Manual RX): row 0 is then the first sync of the lock */
void sstv_rows_init(sstv_rows_t *r, sstv_mode_t mode, double row0_sync);

/* Can row r->next be drawn from the written samples?  Then [*from, *to) is
 * the audio to hand to sstv_rows_draw().  sync: follower status or NULL. */
bool sstv_rows_ready(sstv_rows_t *r, long written, const sstv_sync_status_t *sync, long *from, long *to);

/* Draws the row prepared by sstv_rows_ready() from audio = capture[from..to)
 * into rgb_row (width * 3 bytes).  Robot 36 rows are grey (its colour comes
 * every other line).  Returns the row, or -1 if it could not be drawn. */
int sstv_rows_draw(sstv_rows_t *r, const float *audio, uint8_t *rgb_row);

static inline bool sstv_rows_done(const sstv_rows_t *r) { return r->next >= r->height; }
