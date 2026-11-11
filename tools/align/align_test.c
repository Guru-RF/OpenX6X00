/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  align_int as the main knob uses it (screen_main.c freq_shift: align_int(old + df, abs(df))), at the steps it
 *  takes since #234 and with acceleration, and as RTTY's centre does
 */
#include "util.h"

#include <stdio.h>

static int checks, failed;

static void check(const char *what, int32_t got, int32_t want) {
    checks++;
    if (got != want) {
        failed++;
        printf("FAIL %s: %d, want %d\n", what, (int)got, (int)want);
    }
}

int main(void) {
    /* 100 kHz a detent: on the 100 kHz grid, both ways (the bench X6100 read 6.892.800 for the first) */
    check("7.020 down 100 kHz", align_int(7020000 - 100000, 100000), 6900000);
    check("6.900 down 100 kHz", align_int(6900000 - 100000, 100000), 6800000);
    check("7.020 up 100 kHz", align_int(7020000 + 100000, 100000), 7100000);
    check("14.074 up 100 kHz", align_int(14074000 + 100000, 100000), 14100000);
    /* ten detents read at once: a 1 MHz move on the 1 MHz grid */
    check("7.020 down 1 MHz", align_int(7020000 - 1000000, 1000000), 6000000);
    /* 25 and 10 kHz */
    check("7.020 up 25 kHz", align_int(7020000 + 25000, 25000), 7025000);
    check("145.5125 up 25 kHz", align_int(145512500 + 25000, 25000), 145525000);
    check("7.0231 up 10 kHz", align_int(7023100 + 10000, 10000), 7030000);
    /* 5 kHz turned fast: Strong x15 = 75 kHz, Lite x6 = 30 kHz */
    check("7.1234 down 75 kHz", align_int(7123456 - 75000, 75000), 6975000);
    check("7.1234 up 30 kHz", align_int(7123456 + 30000, 30000), 7140000);
    /* the small steps as before */
    check("7.0201 up 100 Hz", align_int(7020150 + 100, 100), 7020200);
    check("7.02015 up 10 Hz", align_int(7020155 + 10, 10), 7020160);
    check("RTTY centre", align_int(1234, 10), 1230);
    check("step 0 leaves it", align_int(7020155, 0), 7020155);
    /* the largest: 100 kHz x 15 detents read at once on a WR12 frequency */
    check("433.5 down 1.5 MHz", align_int(433500000 - 1500000, 1500000), 432000000);

    printf("align: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
