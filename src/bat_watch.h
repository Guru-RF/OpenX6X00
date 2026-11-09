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

#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Voltages in 0.1 V; LOW + HYSTERESIS = HIGH (upstream 21c2b93) */
#define BAT_LOW         BOARD_BAT_LOW   /* the power-off: 6.0 V, the X6200 6.8 V (board.h) */
#define BAT_HYSTERESIS  2
#define BAT_HIGH        (BAT_LOW + BAT_HYSTERESIS)
#define BAT_LOW_READS   3       /* in a row, about 1.2 s */
#define BAT_NONE        30      /* under 3.0 V: no battery, none fitted or nothing measured yet (clock.c) */
#define BAT_EXT         90      /* on the jack: external power whatever the flags say */
#define BAT_SETTLE_MS   15000   /* after the first reading with a voltage: twice the climb #17 saw */
#define BAT_HELD_MS     5000    /* the X6200: DC IN at BOARD_BAT_EXT_MIN so long holds the battery up (bat_watch.c) */

typedef struct {
    uint64_t settle_at;     /* the readings count from then on; 0 before the first with a voltage */
    uint8_t  low_reads;     /* readings in a row of a battery at BAT_LOW or under */
    bool     low;           /* low battery: the power-off runs */
    bool     counts;        /* the last reading is the battery's and the supply's */
    bool     ext;           /* the last reading: on external power */
    bool     by_volts;      /* the % shown is the voltage's, not the base's (bat_watch_cap, bat_watch_cap_ext) */
    uint8_t  cap_vbat;      /* the voltage the voltage's % is of (bat_watch_cap, bat_watch_cap_ext); 0 none yet */
    uint8_t  cap_ext;       /* on external power: the most the % shown goes to while by_volts (bat_watch_cap_ext) */
    uint8_t  up_reads;      /* readings in a row over cap_vbat */
    bool     held;          /* the X6200: on a supply that holds the battery up (bat_watch.c) */
    uint64_t over_since;    /* the X6200: the first of the readings in a row at BOARD_BAT_EXT_MIN and more; 0 none */
    uint8_t  under_reads;   /* the X6200: receive readings in a row under BOARD_BAT_EXT_MIN */
} bat_watch_t;

/* One reading; bat_missing: the base found no battery (the X6200's says so), tx: the base transmits. True when low
 * changed */
bool bat_watch_reading(bat_watch_t *w, uint8_t vbat, uint8_t vext, bool vext_flag, bool charging, bool bat_missing,
                       bool tx, uint64_t now_ms);

/* The % of a pack at vbat (0.1 V) on battery and receive: what an X6100 base (1.1.8) reports there,
 * from its discharge 8.3 to 6.4 V on the bench */
uint8_t bat_cap_of_volts(uint8_t vbat);

/* The % to show on battery, on receive: the base's, and the voltage's while the two are more than
 * BAT_CAP_APART apart, until they meet within BAT_CAP_MET with the voltage's at BAT_CAP_STEEP or more.
 * The base counts the charge it sees: a pack charged with the radio off stayed near empty at 8.2 V,
 * in Xiegu's firmware too (#49). From 7.3 V down the voltage's falls through 24, 9 and 3 to 0, where
 * it meets a base stuck at 0 as well as one that counts right: there it does not go back to the base's.
 * Nor is a stuck base 25 apart there: the voltage's is also taken when the base says 0 % with the
 * voltage's at BAT_CAP_AT_ZERO or more (7.2 V and up), where one that counts right still had some */
#define BAT_CAP_APART   25
#define BAT_CAP_MET     10
#define BAT_CAP_STEEP   25
#define BAT_CAP_AT_ZERO 9

/* The voltage's % follows a voltage down at once, back up 0.1 V only after BAT_CAP_UP_READS readings in a row over
 * it (about 1 min), 0.2 V or more at once: on a 0.1 V step the readings go back and forth, and the % with them,
 * 24 and 9 % in turn at 7.3 and 7.2 V (#160). From the first reading again when external power comes or goes */
#define BAT_CAP_UP_READS 150
uint8_t bat_watch_cap(bat_watch_t *w, uint8_t vbat, uint8_t batcap);

/* The % to show on external power. There the voltage says little: charging, the pack reads higher than on battery, on
 * the bench X6100 7.6 V with the base at 21-46 % (65 % on battery). So the base's, with two exceptions for a base
 * that has lost count. From BAT_CAP_TOP up the pack is full or close (the bench X6100's sat at 8.3 V on its supply,
 * the base at 99 %, also on TX at 10 W): the voltage's when the base's is more than BAT_CAP_APART under it, as #223's
 * full pack, charged with the radio off, had the base at 2 % on the supply. And plugged in with the voltage's % shown,
 * that % stays: a charge only adds to it. Under BAT_CAP_TOP no more than that one, or than the last from BAT_CAP_TOP
 * up, and the voltage's where it is lower (a supply that does not hold the battery up), down at once and back up as on
 * battery (BAT_CAP_UP_READS); back to the base's once it is within BAT_CAP_MET */
#define BAT_CAP_TOP     82
uint8_t bat_watch_cap_ext(bat_watch_t *w, uint8_t vbat, uint8_t batcap);

#ifdef __cplusplus
}
#endif
