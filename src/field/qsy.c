/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops re-spot after a QSY
 */

#include "qsy.h"

bool field_qsy_step(field_qsy_t *q, bool on, bool moved, bool tx, time_t now) {
    if (!q->spot_at || !moved) {
        q->qsy_at = 0;   /* back where the spot says */
        q->tx     = false;
        return false;
    }
    if (!q->qsy_at)
        q->qsy_at = now;
    if (tx)
        q->tx = true;
    return on && q->tx && now - q->qsy_at >= QSY_SETTLE_S && now - q->spot_at >= QSY_MIN_GAP_S;
}
