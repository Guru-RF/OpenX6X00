/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the panel stripes fix's gamma table (src/tcon_gamma.h, #239), on a fake TCON page that
 *  acts as the processor's did on the radio it was tried on: the table full of random values until filled, an entry
 *  read while the table is on gives 0xC0000000. Table A itself (8..31 to 32 in each channel, the rest as it is); with
 *  the TCON off nothing is read but the global control and nothing written; on: every entry filled while the table is
 *  off, then the table on, again so at a second time (a wake) with the table off first; off: bit 30 cleared and
 *  nothing else; the global control's other bits kept; what the watch sees after the kernel set the engine up again.
 *  See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The fake page: what was written, what a read gives, the writes to the global control in order */
static uint32_t page[1024];
static int      gctl_writes;
static uint32_t gctl_written[8];
static int      table_writes, table_writes_on, reads_other;

static uint32_t fake_rd(volatile uint32_t *t, uint32_t off) {
    (void)t;
    if (off >= 0x400 && off < 0x800 && (page[0] & (1u << 30))) {
        return 0xC0000000u;     /* as the radio gave it with the table on */
    }
    if (off != 0) {
        reads_other++;
    }
    return page[off / 4];
}

static void fake_wr(volatile uint32_t *t, uint32_t off, uint32_t v) {
    (void)t;
    if (off == 0) {
        if (gctl_writes < 8) {
            gctl_written[gctl_writes] = v;
        }
        gctl_writes++;
    } else if (off >= 0x400 && off < 0x800) {
        table_writes++;
        if (page[0] & (1u << 30)) {
            table_writes_on++;
        }
    }
    page[off / 4] = v;
}

#define TCON_RD(t, off)     fake_rd(t, off)
#define TCON_WR(t, off, v)  fake_wr(t, off, v)
#include "tcon_gamma.h"

static int checks, failed;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* The page as at a start: this global control, the table random (as 0x00470BAF, 0x001D0FA5 read on the radio) */
static void fresh(uint32_t gctl) {
    uint32_t x = 0x00470BAFu;

    memset(page, 0, sizeof(page));
    for (int i = 0; i < TCON_GAMMA_N; i++) {
        x = x * 1103515245u + 12345u;
        page[0x100 + i] = x & 0x00FFFFFFu;
    }
    page[0] = gctl;
    gctl_writes = table_writes = table_writes_on = reads_other = 0;
}

/* Every entry as table A has it, read from the page itself (the fake's reads hide a table that is on) */
static bool table_a(void) {
    for (int i = 0; i < TCON_GAMMA_N; i++) {
        if (page[0x100 + i] != tcon_gamma_entry(i)) {
            return false;
        }
    }
    return true;
}

int main(void) {
    volatile uint32_t *t = page;
    uint32_t           before[TCON_GAMMA_N];
    bool               same = true;

    /* Table A */
    check(tcon_gamma_entry(0) == 0x000000 && tcon_gamma_entry(7) == 0x070707, "0..7 as they are");
    for (int i = STRIPES_LOW; i <= STRIPES_HIGH; i++) {
        same = same && tcon_gamma_entry(i) == 0x202020;
    }
    check(same, "8..31 out as 32 in R, G and B");
    same = true;
    for (int i = 32; i < 256; i++) {
        same = same && tcon_gamma_entry(i) == ((uint32_t)i << 16 | (uint32_t)i << 8 | (uint32_t)i);
    }
    check(same, "32..255 as they are");
    check(tcon_gamma_a(20) == 32 && tcon_gamma_a(31) == 32 && tcon_gamma_a(48) == 48 && tcon_gamma_a(255) == 255,
          "one channel: 20 and 31 to 32, 48 and 255 kept");

    /* The TCON off: nothing */
    fresh(0x00000000u);
    memcpy(before, page + 0x100, sizeof(before));
    check(tcon_gamma_set(t, true) == TCON_GAMMA_NO_TCON, "TCON off: on does nothing");
    check(gctl_writes == 0 && table_writes == 0 && reads_other == 0 && !memcmp(before, page + 0x100, sizeof(before)),
          "TCON off: nothing written, nothing read but the global control");
    check(tcon_gamma_set(t, false) == TCON_GAMMA_NO_TCON && gctl_writes == 0, "TCON off: off does nothing either");

    /* On, as on the radio it was tried on: 0x80000000, the table random */
    fresh(0x80000000u);
    check(tcon_gamma_set(t, true) == TCON_GAMMA_ON, "on");
    check(table_a(), "every entry table A");
    check(table_writes == TCON_GAMMA_N && table_writes_on == 0, "256 entries written, none with the table on");
    check(gctl_writes == 1 && gctl_written[0] == 0xC0000000u && page[0] == 0xC0000000u,
          "then the table on: 0xC0000000, one write");

    /* Again with it on (a wake, the setting turned on twice): off first, filled, on */
    gctl_writes = table_writes = table_writes_on = 0;
    check(tcon_gamma_set(t, true) == TCON_GAMMA_ON && table_a(), "on again: table A");
    check(gctl_writes == 2 && gctl_written[0] == 0x80000000u && gctl_written[1] == 0xC0000000u,
          "on again: the table off, then on");
    check(table_writes == TCON_GAMMA_N && table_writes_on == 0, "on again: no entry written with the table on");

    /* Off */
    gctl_writes = table_writes = 0;
    check(tcon_gamma_set(t, false) == TCON_GAMMA_OFF, "off");
    check(gctl_writes == 1 && page[0] == 0x80000000u && table_writes == 0, "off: bit 30 cleared, the table left");
    gctl_writes = 0;
    check(tcon_gamma_set(t, false) == TCON_GAMMA_OFF && gctl_writes == 0, "off when off: nothing written");

    /* The other bits of the global control kept (the kernel's pin map in the low bits) */
    fresh(0x80000001u);
    tcon_gamma_set(t, true);
    check(page[0] == 0xC0000001u, "on: the other bits kept");
    tcon_gamma_set(t, false);
    check(page[0] == 0x80000001u, "off: the other bits kept");

    /* What the watch sees */
    fresh(0xC0000000u);
    check(!tcon_gamma_lost(t), "the table on: not lost");
    fresh(0x80000000u);
    check(tcon_gamma_lost(t), "the TCON on, the table off: lost");
    fresh(0x00000000u);
    check(!tcon_gamma_lost(t), "the TCON off: not lost, nothing to do until it is on");
    fresh(0x40000000u);
    check(!tcon_gamma_lost(t), "bit 30 alone: not lost");

    /* The kernel set the engine up again: the global control 0 then the TCON on, the table random */
    fresh(0x80000000u);
    tcon_gamma_set(t, true);
    fresh(0x80000000u);
    check(tcon_gamma_lost(t) && tcon_gamma_set(t, true) == TCON_GAMMA_ON && table_a() && page[0] == 0xC0000000u,
          "after a reset: seen as lost, table A on again");

    printf("tcon_gamma: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
