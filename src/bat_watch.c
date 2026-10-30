/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the base's supply readings (bat_watch.h)
 */

#include "bat_watch.h"

#include <stdlib.h>

bool bat_watch_reading(bat_watch_t *w, uint8_t vbat, uint8_t vext, bool vext_flag, bool charging, uint64_t now_ms) {
    bool measured = vbat >= BAT_NONE || vext >= BAT_NONE;

    /* External power: the flag alone is not enough, charging an empty battery it stayed clear with
     * 11.7 V on the jack and the radio switched itself off on the charger */
    w->ext = vext_flag || charging || vext >= BAT_EXT;
    if (measured && !w->settle_at) {
        w->settle_at = now_ms + BAT_SETTLE_MS;
    }
    w->counts = measured && w->settle_at && now_ms >= w->settle_at;

    /* Only on a change, with hysteresis: around 6.0 V the 30 s power-off timer was started and
     * stopped over and over. Low only after 3 readings in a row: one of the base's first packets
     * (0 V on both) alone started "Low battery!" with a full battery on the charger. And only on a
     * battery that is there and measured: under 3 V there is none, or no reading yet */
    bool battery_low = w->counts && !w->ext && vbat >= BAT_NONE && vbat <= BAT_LOW;

    w->low_reads = battery_low ? (w->low_reads < BAT_LOW_READS ? w->low_reads + 1 : BAT_LOW_READS) : 0;

    bool enter = w->low_reads >= BAT_LOW_READS;
    bool leave = w->ext || vbat >= BAT_HIGH;

    if ((!w->low && enter) || (w->low && leave)) {
        w->low = !w->low;
        return true;
    }
    return false;
}

uint8_t bat_cap_of_volts(uint8_t vbat) {
    /* 0.1 V from 6.8 V up: the median % the base reported at each, on battery and receive */
    static const uint8_t cap[] = {0, 1, 1, 3, 9, 24, 42, 56, 65, 73, 80, 84, 88, 94, 98, 100};

    if (vbat <= 68) return 0;
    if (vbat >= 68 + sizeof(cap) - 1) return 100;
    return cap[vbat - 68];
}

uint8_t bat_watch_cap(bat_watch_t *w, uint8_t vbat, uint8_t batcap) {
    uint8_t v = bat_cap_of_volts(vbat);
    int     apart = abs((int)batcap - v);

    /* #49's base (1.1.9) said 0-3 % from 8.2 V down. Back to it at the voltage's 9 % (7.2 V), the radio said
     * 0 % and "connect power" every minute for the 35 to 42 minutes the pack still had */
    if (apart > BAT_CAP_APART) w->by_volts = true;
    else if (apart <= BAT_CAP_MET && v >= BAT_CAP_STEEP) w->by_volts = false;
    return w->by_volts ? v : batcap;
}
