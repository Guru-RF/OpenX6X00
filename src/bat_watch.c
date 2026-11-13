/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the base's supply readings (bat_watch.h)
 */

#include "bat_watch.h"

#include <stdlib.h>

bool bat_watch_reading(bat_watch_t *w, uint8_t vbat, uint8_t vext, bool vext_flag, bool charging, bool bat_missing,
                       bool tx, uint64_t now_ms) {
    bool measured = vbat >= BAT_NONE || vext >= BAT_NONE;
    bool was_ext  = w->ext;

    /* External power: the flag alone is not enough, charging an empty battery it stayed clear with
     * 11.7 V on the jack and the radio switched itself off on the charger */
    w->ext = vext_flag || charging || vext >= BAT_EXT;
    if (measured && !w->settle_at) {
        w->settle_at = now_ms + BAT_SETTLE_MS;
    }
    w->counts = measured && w->settle_at && now_ms >= w->settle_at;
    /* The voltage's % from the next reading as it is, on battery (bat_watch_cap) and on external power
     * (bat_watch_cap_ext). Plugged in with the voltage's % shown, that one on the supply */
    if (w->ext != was_ext || !w->counts) {
        w->cap_ext  = w->ext && w->by_volts && w->cap_vbat ? bat_cap_of_volts(w->cap_vbat) : 0;
        w->cap_vbat = 0;
    }

    /* External power that holds the battery up: on the X6200 only DC IN at BOARD_BAT_EXT_MIN and more (board.h),
     * under it the battery ran the radio down to the base's cut with the flags saying external power. With
     * hysteresis: DC IN comes in 0.1 V steps, and taken from each reading a supply close to 12.0 V started the
     * power-off and called it off again over and over, or with every other reading at 12.0 V never started it.
     * Held after BAT_HELD_MS of readings in a row at BOARD_BAT_EXT_MIN and more, at once from 0.2 V over; no longer
     * after BAT_LOW_READS receive readings in a row under it, at once with no supply at all. The readings on TX
     * don't take it down: the supply sags under the TX current (the DCIN TOO LOW blink) */
#ifdef BOARD_BAT_EXT_MIN
    if (vext >= BOARD_BAT_EXT_MIN) {
        w->under_reads = 0;
        if (!w->over_since) w->over_since = now_ms;
        if (vext >= BOARD_BAT_EXT_MIN + BAT_HYSTERESIS || now_ms - w->over_since >= BAT_HELD_MS) w->held = true;
    } else {
        w->over_since = 0;
        if (!tx && w->under_reads < BAT_LOW_READS) w->under_reads++;
        if (w->under_reads >= BAT_LOW_READS || vext < BAT_NONE) w->held = false;
    }
    bool held = w->held;
#else
    (void)tx;
    bool held = w->ext;
#endif

    /* Only on a change, with hysteresis: around BAT_LOW the 30 s power-off timer was started and
     * stopped over and over. Low only after 3 readings in a row: one of the base's first packets
     * (0 V on both) alone started "Low battery!" with a full battery on the charger. And only on a
     * battery that is there and measured: under 3 V there is none, or no reading yet. Nor when the base finds none
     * (the X6200's says so): a supply under BOARD_BAT_EXT_MIN does not stop it there, and what vbat says with no
     * pack has not been seen (clock.c takes the two apart); a radio on its stock charger without one stays on */
    bool battery_low = w->counts && !held && !bat_missing && vbat >= BAT_NONE && vbat <= BAT_LOW;

    w->low_reads = battery_low ? (w->low_reads < BAT_LOW_READS ? w->low_reads + 1 : BAT_LOW_READS) : 0;

    bool enter = w->low_reads >= BAT_LOW_READS;
    bool leave = held || bat_missing || vbat >= BAT_HIGH;

    if ((!w->low && enter) || (w->low && leave)) {
        w->low = !w->low;
        return true;
    }
    return false;
}

bool bat_supply_reading(bat_supply_t *s, const bat_watch_t *w, uint64_t now_ms) {
    if (w->counts && w->ext == s->seen_bat) {
        s->seen_bat = !w->ext;
        s->seen_ms  = now_ms;
    }
    if (s->seen_bat == s->bat || now_ms - s->seen_ms < BAT_SUPPLY_MS) return false;
    s->bat = s->seen_bat;
    return true;
}

uint8_t bat_cap_of_volts(uint8_t vbat) {
    /* 0.1 V from 6.8 V up: the median % the base reported at each, on battery and receive */
    static const uint8_t cap[] = {0, 1, 1, 3, 9, 24, 42, 56, 65, 73, 80, 84, 88, 94, 98, 100};

    if (vbat <= 68) return 0;
    if (vbat >= 68 + sizeof(cap) - 1) return 100;
    return cap[vbat - 68];
}

/* The voltage the voltage's % is of: down at once, up a step after BAT_CAP_UP_READS readings over it (bat_watch.h) */
static uint8_t cap_volts(bat_watch_t *w, uint8_t vbat) {
    w->up_reads = vbat > w->cap_vbat && w->up_reads < BAT_CAP_UP_READS ? w->up_reads + 1 : 0;
    if (!w->cap_vbat || vbat < w->cap_vbat || vbat > w->cap_vbat + 1 || w->up_reads >= BAT_CAP_UP_READS) {
        w->cap_vbat = vbat;
        w->up_reads = 0;
    }
    return w->cap_vbat;
}

uint8_t bat_watch_cap(bat_watch_t *w, uint8_t vbat, uint8_t batcap) {
    /* Back up a step only once the battery stays there: after a TX, which radio.c holds the % over for 30 s, it
     * can still be coming back */
    uint8_t v = bat_cap_of_volts(cap_volts(w, vbat));
    int     apart = abs((int)batcap - v);

    /* #49's base (1.1.9) said 0-3 % from 8.2 V down. Back to it at the voltage's 9 % (7.2 V), the radio said
     * 0 % and "connect power" every minute for the 35 to 42 minutes the pack still had. Switched on at 7.3 or
     * 7.2 V, the two were never 25 apart and it said that from the first reading: a base at 0 % there has lost
     * count */
    if (apart > BAT_CAP_APART || (batcap == 0 && v >= BAT_CAP_AT_ZERO)) w->by_volts = true;
    else if (apart <= BAT_CAP_MET && v >= BAT_CAP_STEEP) w->by_volts = false;
    return w->by_volts ? v : batcap;
}

uint8_t bat_watch_cap_ext(bat_watch_t *w, uint8_t vbat, uint8_t batcap) {
    if (vbat < BAT_NONE) return batcap;     /* no battery: clock.c shows the supply alone */

    /* On the 8.2/8.3 V step the % of the lower one, as on battery. Under it cap_ext, or the voltage's where that is
     * lower, back up after a dip as on battery: a % that only went down kept a dip of one reading for good */
    bool    top = cap_volts(w, vbat) >= BAT_CAP_TOP;
    uint8_t v   = bat_cap_of_volts(w->cap_vbat);

    if (top) w->cap_ext = v;
    if (top && v > batcap + BAT_CAP_APART) w->by_volts = true;
    v = v < w->cap_ext ? v : w->cap_ext;
    if (w->by_volts && batcap + BAT_CAP_MET >= v) w->by_volts = false;
    return w->by_volts ? v : batcap;
}
