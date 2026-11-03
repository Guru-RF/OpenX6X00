/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the WR12 receiver on the X6200's HOST port (wr12.c, #134). The X6100 has none: there the serial
 *  paddle's question below is all that is left, and it says no
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where the base listens to what the WR12 receives out of its bypass */
#define WR12_IF_HZ  5060000

#ifdef BOARD_X6200

/* The thread that has the WR12's port, idle while WR12 on HOST is off. notify: called on that thread when what
 * wr12_ready, wr12_tx_block or wr12_note say may have changed; false (the GUI's queue full) calls it again later.
 * keyed: the radio is in TX; the WR12 leaves its bypass only once it is not */
void wr12_init(bool (*notify)(void), bool (*keyed)(void));

/* What the WR12 is to do, from any thread, never waiting for it. on: WR12 on HOST (off: the WR12 back in its bypass,
 * then its port let go); rx: out of its bypass, tuned to hz (radio.c asks it only with TX held); not rx: in its
 * bypass, the antenna straight through to the radio */
void wr12_want(bool on, bool rx, int32_t hz);

/* Found and set up: the base may go onto its IF */
bool wr12_ready(void);

/* Why nothing may transmit now, NULL when the WR12 has nothing against it: asked out of its bypass, or not seen back in
 * it (no OK to AT+BYPASS=1 since AT+BYPASS=0, and still plugged in). Any thread */
const char *wr12_tx_block(void);

/* The next line for the screen (found, lost, an ERROR answer), into buf; false: none */
bool wr12_note(char *buf, size_t size);

/* dev ("/dev/ttyACM0") is the WR12's port and WR12 on HOST is on: the serial paddle leaves it alone */
bool wr12_port(const char *dev);

#else

static inline bool wr12_port(const char *dev) {
    (void)dev;
    return false;
}

#endif
