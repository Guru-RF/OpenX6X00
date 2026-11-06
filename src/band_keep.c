/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The bands an app tunes across, left as it found them (band_keep.h)
 */
#include "band_keep.h"

#include "cfg/cfg_api.h"
#include "screen_main.h"

#include <stdbool.h>

#define KEPT_MAX 40     /* more bands than the band list has */

static cfg_band_dial_t kept[KEPT_MAX];
static int             kept_n;
static int32_t         own = -1;   /* the band in use when the app opened: the backup memory's */
static bool            on;

void band_keep_begin(void) {
    kept_n = 0;
    own    = cfg_band_current();
    on     = true;
}

void band_keep_tune(int32_t freq) {
    if (!on) return;
    int32_t id = cfg_band_of(freq);

    if (id < 0 || id == own) return;
    for (int i = 0; i < kept_n; i++)
        if (kept[i].band_id == id) return;
    if (kept_n < KEPT_MAX && cfg_band_dial_get(freq, &kept[kept_n])) kept_n++;
}

void band_keep_end(void) {
    if (!on) return;
    on = false;
    if (!kept_n) return;
    mem_load(MEM_BACKUP_ID);    /* off the bands kept, onto the band it opened on */
    for (int i = 0; i < kept_n; i++) cfg_band_dial_put(&kept[i]);
    kept_n = 0;
}
