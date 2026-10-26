/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the base's supply readings (bat_watch.h)
 */

#include "bat_watch.h"

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
