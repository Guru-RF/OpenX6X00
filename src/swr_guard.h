/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - SWR protection: a transmission whose SWR stays over the limit for
 *  SWR_GUARD_MS is stopped (radio.c). Key-up spikes are shorter; not while the ATU tunes.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWR_GUARD_MS     300
#define SWR_GUARD_GAP_MS 1000   /* no reading for longer: the next one starts a new transmission */

typedef struct {
    bool     high;      /* over the limit since `since` */
    uint64_t since;
    uint64_t last;      /* the last reading */
} swr_guard_t;

/* One reading while transmitting; limit 1.0 or less: off. True: stop the TX now */
bool swr_guard_check(swr_guard_t *g, float pwr, float swr, float limit, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
