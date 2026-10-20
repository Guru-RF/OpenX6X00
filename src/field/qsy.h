/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: when an activation spots again after a QSY. Only once the
 *  operator has transmitted on the new frequency or mode and stayed there: tuning around and
 *  listening is not a QSY.
 */

#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QSY_SETTLE_S  60    /* a minute on the new frequency: working there, not looking for a gap */
#define QSY_MIN_GAP_S 300   /* at most one auto spot in 5 minutes: the spot sites are not flooded */

typedef struct {
    time_t spot_at;   /* the last spot, 0: none yet */
    time_t qsy_at;    /* when we left the spotted frequency/mode, 0: still there */
    bool   tx;        /* transmitted since then */
} field_qsy_t;

/* Called every tick; moved: frequency 1 kHz away or another mode; tx: transmitting now.
 * True when it is time to spot again (on: the "Re-spot on QSY" option). */
bool field_qsy_step(field_qsy_t *q, bool on, bool moved, bool tx, time_t now);

#ifdef __cplusplus
}
#endif
