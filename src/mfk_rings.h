/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the two rings of the X6200's lower knob (#354)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

/* The X6200's lower knob has two rings, each a device of its own. One of them is the MFK proper (it changes the
 * value or chooses the control on the main screen, and walks the items of apps, lists, menus and Settings), the other
 * steps the MFK's control on the main screen and does nothing elsewhere. Settings > Display > INTERFACE > MFK rings
 * says which is which: Stock (the outer ring is the MFK, the inner one steps the control, as the maker's firmware) or
 * Swapped. The X6100's MFK has one ring: the outer one here, always the MFK.
 *
 * What a ring's device sends becomes clicks here: mfk_ring_scale[] below is THE place for a ring's direction and
 * its counts per click in each use. Only the header, so the host test (tools/mfk_rings) takes it alone. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { MFK_RING_OUTER, MFK_RING_INNER, MFK_RINGS } mfk_ring_t;
typedef enum { MFK_ROLE_KNOB, MFK_ROLE_STEP, MFK_ROLES } mfk_role_t;   /* the MFK proper; the step of its control */

/* dir: +1 when the device counts a turn to the left as positive, as VOL and the outer ring do; -1 turns it round.
 * div: the device's counts in one click of the ring.
 *
 * Known from the radio: the outer ring as the MFK and the inner ring as the step, one count a click, left positive.
 * NOT checked on a radio: the other two, taken to be the same as the ring's known use. If Swapped turns the wrong
 * way or moves several items a click, the INNER / KNOB line is the one to change (and OUTER / STEP for the step) */
static const struct {
    int8_t  dir;
    uint8_t div;
} mfk_ring_scale[MFK_RINGS][MFK_ROLES] = {
    [MFK_RING_OUTER] = {[MFK_ROLE_KNOB] = {+1, 1}, [MFK_ROLE_STEP] = {+1, 1}},
    [MFK_RING_INNER] = {[MFK_ROLE_KNOB] = {+1, 1}, [MFK_ROLE_STEP] = {+1, 1}},
};

/* The ring that has a role */
static inline mfk_ring_t mfk_ring_of(mfk_role_t role, bool swapped) {
    return (role == MFK_ROLE_STEP) != swapped ? MFK_RING_INNER : MFK_RING_OUTER;
}

/* Counts as clicks, a turn to the left positive: dir is +1 or -1, div the counts in a click (0 as 1). *rest keeps
 * the counts short of a click for the next read (zero it when the ring changes role) */
static inline int32_t mfk_ring_scaled(int32_t counts, int32_t dir, int32_t div, int32_t *rest) {
    int32_t sum = *rest + counts * dir;
    int32_t clicks = sum / (div ? div : 1);

    *rest = sum - clicks * (div ? div : 1);
    return clicks;
}

/* A ring's counts as clicks of its role */
static inline int32_t mfk_ring_clicks(mfk_ring_t ring, mfk_role_t role, int32_t counts, int32_t *rest) {
    return mfk_ring_scaled(counts, mfk_ring_scale[ring][role].dir, mfk_ring_scale[ring][role].div, rest);
}
