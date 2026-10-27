/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the base's supply readings as radio.c takes them, about every 0.4 s: whether they
 *  are the battery's yet, external power, and the low battery that starts the 30 s power-off
 *  (screen_main.c).
 *
 *  Seen on one X6100 on battery (#17), by its owner: after power-on its base sent about 3 s of
 *  nothing measured (0 V on both), then a % that climbed to the battery's over about 7 s more.
 *  Other bases, the X6200's among them, have not been watched doing it, and none of it was logged.
 *  Such readings are not the battery's. The top bar, the battery log with the time left, its
 *  10 % warning and the low battery take readings from BAT_SETTLE_MS after the first one with a
 *  voltage, which covers that sequence and costs a base that reports at once only the wait.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Voltages in 0.1 V; LOW + HYSTERESIS = HIGH (upstream 21c2b93) */
#define BAT_LOW         60      /* the power-off */
#define BAT_HYSTERESIS  2
#define BAT_HIGH        (BAT_LOW + BAT_HYSTERESIS)
#define BAT_LOW_READS   3       /* in a row, about 1.2 s */
#define BAT_NONE        30      /* under 3.0 V: no battery, none fitted or nothing measured yet (clock.c) */
#define BAT_EXT         90      /* on the jack: external power whatever the flags say */
#define BAT_SETTLE_MS   15000   /* after the first reading with a voltage: twice the climb #17 saw */

typedef struct {
    uint64_t settle_at;     /* the readings count from then on; 0 before the first with a voltage */
    uint8_t  low_reads;     /* readings in a row of a battery at BAT_LOW or under */
    bool     low;           /* low battery: the power-off runs */
    bool     counts;        /* the last reading is the battery's and the supply's */
    bool     ext;           /* the last reading: on external power */
    bool     by_volts;      /* the % shown is the voltage's, not the base's (bat_watch_cap) */
} bat_watch_t;

/* One reading. True when low changed */
bool bat_watch_reading(bat_watch_t *w, uint8_t vbat, uint8_t vext, bool vext_flag, bool charging, uint64_t now_ms);

/* The % of a pack at vbat (0.1 V) on battery and receive: what an X6100 base (1.1.8) reports there,
 * from its discharge 8.3 to 6.4 V on the bench */
uint8_t bat_cap_of_volts(uint8_t vbat);

/* The % to show on battery, on receive: the base's, and the voltage's while the two are more than
 * BAT_CAP_APART apart, until they meet within BAT_CAP_MET. The base counts the charge it sees: a
 * pack charged with the radio off stayed near empty at 8.2 V, in Xiegu's firmware too (#49) */
#define BAT_CAP_APART   25
#define BAT_CAP_MET     10
uint8_t bat_watch_cap(bat_watch_t *w, uint8_t vbat, uint8_t batcap);

#ifdef __cplusplus
}
#endif
