/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Battery time left: the pack's discharge curve (minutes per 10%) scaled by the
 *  load (TX watts, WiFi, Bluetooth, headphones, brightness) and by how long this
 *  pack really lasts, learned from the battery log; time to full from the charge
 *  rate seen at each 10% of the pack.
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    time_t  t;
    float   vbat;
    int     cap;        /* the base's % */
    float   vext;
    bool    charging;
    float   tx_s;       /* seconds of TX since the previous sample */
    float   tx_w;       /* average output power while transmitting */
    int     bl;         /* backlight level, -1 screen off */
    bool    wifi, bt, hp;
} bat_sample_t;

void bat_est_feed(const bat_sample_t *s);

/* Minutes to to_cap % on battery (0: empty) or to full while charging, -1 when there is
 * nothing to show. approx: the model has seen little of this yet */
int  bat_est_minutes(int to_cap, bool *charging, bool *approx);

bool bat_est_load(const char *path);
bool bat_est_save(const char *path);

/* Feeds a battery_log.csv, old (8 columns) or new layout */
bool bat_est_replay(const char *path);

void bat_est_reset(void);

/* What the model has learned from: hours on battery, charges seen to the end */
void bat_est_learned(int *hours, int *charges);

/* How much longer than the stock curve this pack lasts (1 = as the curve) */
float bat_est_factor(void);

#ifdef __cplusplus
}
#endif
