/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the clock's rule (clock_sync.h): which source keeps the clock, which may move it, ntpd's word, a
 *  learned value. Nothing here reads a clock or a file: the host test (tools/clock_sync) drives it.
 */

#include "clock_sync.h"

#include <math.h>

clock_by_t clock_sync_keeper_of(bool gps_recent, int ntpd, bool kernel_synced) {
    if (gps_recent)
        return CLOCK_BY_GPS;
    if (ntpd > 0 || (ntpd < 0 && kernel_synced))
        return CLOCK_BY_NTP;
    return CLOCK_BY_NONE;
}

bool clock_sync_may_step(clock_by_t by, clock_by_t keeper, clock_by_t last_by, long since, bool auto_sync) {
    if (keeper == CLOCK_BY_GPS || keeper == CLOCK_BY_NTP)
        return false;       /* kept: the others only measure */
    switch (by) {
        case CLOCK_BY_HAND:
        case CLOCK_BY_STATION:
            return true;    /* asked for: Time Sync pressed, a time station listened to */
        case CLOCK_BY_FT8:
            return auto_sync && !(last_by == CLOCK_BY_STATION && since >= 0 && since < CLOCK_SYNC_STATION_HOLD_S);
        default:
            return false;
    }
}

int clock_sync_ntpd_reply(const uint8_t *r, size_t n) {
    /* a response (bit 7 of the op byte) in mode 6, to READVAR (op 2) */
    if (n < 12 || (r[0] & 7) != 6 || !(r[1] & 0x80) || (r[1] & 0x1f) != 2)
        return -1;

    int leap   = r[4] >> 6;     /* the system status word: leap bits, then the sync source */
    int source = r[4] & 0x3f;

    return leap != 3 && source != 0 && source != 5;     /* 0 unspecified, 5 its local clock: no source */
}

bool clock_sync_learn(clock_learned_t *l, float v, float far) {
    if (!isfinite(v))
        return false;
    if (l->n >= 10 && fabsf(v - l->value) > far)
        return false;
    l->value = l->n < 5 ? (l->value * l->n + v) / (l->n + 1) : l->value + 0.05f * (v - l->value);
    l->n++;
    return true;
}

const char *clock_sync_by_name(clock_by_t by) {
    switch (by) {
        case CLOCK_BY_GPS:     return "GPS";
        case CLOCK_BY_NTP:     return "NTP";
        case CLOCK_BY_STATION: return "time station";
        case CLOCK_BY_FT8:     return "FT8";
        case CLOCK_BY_HAND:    return "Time Sync";
        default:               return "clock chip";
    }
}

