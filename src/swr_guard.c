/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - SWR protection (swr_guard.h)
 */

#include "swr_guard.h"

bool swr_guard_check(swr_guard_t *g, float pwr, float swr, float limit, uint64_t now_ms) {
    /* A new transmission; CW's short receive between letters goes on with the last */
    if (now_ms - g->last > SWR_GUARD_GAP_MS) {
        g->high = false;
    }
    g->last = now_ms;

    /* No power (between CW elements): no SWR to read, the time over the limit goes on */
    if (pwr <= 0.0f) {
        return false;
    }
    if (limit <= 1.0f || swr <= limit) {
        g->high = false;
        return false;
    }
    if (!g->high) {
        g->high  = true;
        g->since = now_ms;
        return false;
    }
    if (now_ms - g->since < SWR_GUARD_MS) {
        return false;
    }
    g->high = false;
    return true;
}
