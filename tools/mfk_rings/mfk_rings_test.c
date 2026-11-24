/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the X6200's two MFK rings (src/mfk_rings.h, #354): which ring is the MFK and which steps
 *  its control with MFK rings on Stock and on Swapped; the table of directions and counts per click (the two uses
 *  known from the radio as they are, every entry a direction and at least one count); and counts made clicks: the
 *  direction, a divider that keeps what is short of a click for the next read, both ways and through a reversal. It
 *  does not say what the inner ring sends as the MFK: that needs a radio. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "mfk_rings.h"

#include <stdio.h>

static int checks, failed;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

int main(void) {
    int32_t rest = 0;

    /* Who is who */
    check(mfk_ring_of(MFK_ROLE_KNOB, false) == MFK_RING_OUTER, "Stock: the outer ring is the MFK");
    check(mfk_ring_of(MFK_ROLE_STEP, false) == MFK_RING_INNER, "Stock: the inner ring steps the control");
    check(mfk_ring_of(MFK_ROLE_KNOB, true) == MFK_RING_INNER, "Swapped: the inner ring is the MFK");
    check(mfk_ring_of(MFK_ROLE_STEP, true) == MFK_RING_OUTER, "Swapped: the outer ring steps the control");
    for (int swapped = 0; swapped < 2; swapped++)
        check(mfk_ring_of(MFK_ROLE_KNOB, swapped) != mfk_ring_of(MFK_ROLE_STEP, swapped),
              "the two roles are never the same ring");

    /* The table: Stock's two uses are the radio's as it was read before the setting, one count a click, left
     * positive; whatever the other two are set to after a check on a radio, a direction and a count */
    check(mfk_ring_scale[MFK_RING_OUTER][MFK_ROLE_KNOB].dir == 1 && mfk_ring_scale[MFK_RING_OUTER][MFK_ROLE_KNOB].div == 1,
          "Stock: the outer ring as the MFK is read as before");
    check(mfk_ring_scale[MFK_RING_INNER][MFK_ROLE_STEP].dir == 1 && mfk_ring_scale[MFK_RING_INNER][MFK_ROLE_STEP].div == 1,
          "Stock: the inner ring as the step is read as before");
    for (int ring = 0; ring < MFK_RINGS; ring++) {
        for (int role = 0; role < MFK_ROLES; role++) {
            int dir = mfk_ring_scale[ring][role].dir;

            check(dir == 1 || dir == -1, "a direction is +1 or -1");
            check(mfk_ring_scale[ring][role].div >= 1, "a click is at least one count");
        }
    }

    /* Stock, through the table: every count a click, the sign kept, nothing left over */
    check(mfk_ring_clicks(MFK_RING_OUTER, MFK_ROLE_KNOB, 3, &rest) == 3 && rest == 0, "Stock MFK: 3 counts, 3 clicks");
    check(mfk_ring_clicks(MFK_RING_OUTER, MFK_ROLE_KNOB, -2, &rest) == -2 && rest == 0, "Stock MFK: 2 the other way");
    check(mfk_ring_clicks(MFK_RING_INNER, MFK_ROLE_STEP, 1, &rest) == 1 && rest == 0, "Stock step: 1 count, 1 step");
    check(mfk_ring_clicks(MFK_RING_INNER, MFK_ROLE_STEP, 0, &rest) == 0 && rest == 0, "nothing read: nothing");

    /* The direction turned round */
    check(mfk_ring_scaled(4, -1, 1, &rest) == -4 && rest == 0, "dir -1: 4 counts are 4 clicks the other way");
    check(mfk_ring_scaled(-1, -1, 1, &rest) == 1 && rest == 0, "dir -1: and back");

    /* A divider: two counts a click, what is short of one kept */
    rest = 0;
    check(mfk_ring_scaled(1, 1, 2, &rest) == 0 && rest == 1, "div 2: one count is no click yet");
    check(mfk_ring_scaled(1, 1, 2, &rest) == 1 && rest == 0, "div 2: the second count makes it");
    check(mfk_ring_scaled(5, 1, 2, &rest) == 2 && rest == 1, "div 2: 5 counts are 2 clicks and one kept");
    check(mfk_ring_scaled(-1, 1, 2, &rest) == 0 && rest == 0, "div 2: a count back takes the kept one away");
    check(mfk_ring_scaled(-3, 1, 2, &rest) == -1 && rest == -1, "div 2: 3 counts back are a click and one kept");
    check(mfk_ring_scaled(-1, 1, 2, &rest) == -1 && rest == 0, "div 2: and the next count back makes the second");

    /* Four counts a click and the direction turned round, a slow turn of 8 counts read one at a time: 2 clicks */
    {
        int32_t clicks = 0;

        rest = 0;
        for (int i = 0; i < 8; i++)
            clicks += mfk_ring_scaled(1, -1, 4, &rest);
        check(clicks == -2 && rest == 0, "div 4, dir -1: 8 counts one at a time are 2 clicks the other way");
    }

    /* No divider set (0) is one count a click, not a division by zero */
    rest = 0;
    check(mfk_ring_scaled(3, 1, 0, &rest) == 3 && rest == 0, "div 0 counts as 1");

    printf("mfk_rings: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
