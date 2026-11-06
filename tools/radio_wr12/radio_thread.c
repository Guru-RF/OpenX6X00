/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of radio.c's side of the X6200's WR12 (#134): src/radio.c whole, and what its radio thread
 *  does for the base's open TX (#176) within the test's reach, which runs it in the thread's place: the thread itself
 *  is not started here (fakes.c), so no packet comes, and the base keys by itself only when the test says so. See
 *  the Makefile and fakes.h.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#include "../../src/radio.c"

/* One turn of the radio thread at the base's open TX (open_tx_tick) */
void radio_thread_turn(void) {
    open_tx_tick();
}

/* The base in TX by itself or back in RX, as radio_tick takes it from the packets (the hand mic's PTT, at 0 W too) */
void base_keys_itself(bool tx) {
    atomic_store(&state, tx ? RADIO_TX : RADIO_RX);
}

/* A write of the open TX on its way, as open_tx_send has one on the radio thread: under the lock, open_tx_busy set */
void open_tx_write_begin(void) {
    radio_lock();
    atomic_store(&open_tx_busy, true);
}

void open_tx_write_end(void) {
    atomic_store(&open_tx_busy, false);
    radio_unlock();
}
