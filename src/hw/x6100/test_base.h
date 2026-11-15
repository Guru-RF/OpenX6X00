/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6100 test base "tx1a2-test": RF.Guru's X6100-Baseband test release, Xiegu's V1.1.9 with an
 *  added compressor/gate, CESSB and adjustable TX filter. TEST ONLY: not validated on a radio.
 *
 *  The base takes its added settings only from a GUI that asks for them: register 36 with "TX" (0x5458) in its upper
 *  half lets it use them: the TX filter in every mode, the compressor in voice TX, the CESSB path with its own
 *  interpolation in voice TX with CESSB on (not for the token alone). Each change goes out whole, in this order:
 *  register 36 without the token (the added processing off), register 25 (compressor) and register 38 (TX filter) in
 *  full, then register 36 with the token, last. A setting changed while the radio transmits waits for receive: the
 *  base keeps its settings through a TX, and the token taken away mid-over would switch the processing off under the
 *  voice. Not all at once on the base's side: a change just as a TX starts can still reach it then.
 *
 *  No hardware here: the bits, the identity and the order, tested on the PC (tools/test_base).
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* The whole version string of the test base (register 0): Xiegu's V1.1.9 with its build time replaced. Nothing
 * looser: "V1.1.9" alone is Xiegu's own base, ",rN" the patched bases of x6100_base_patch */
#define TEST_BASE_ID        "V1.1.9 Nov 27 2025,tx1a2-test"

/* The registers (the control library's x6100_cmplevel_cmpe, x6100_cw_peak_cessb, x6100_tx_filter) */
#define TEST_BASE_REG_COMP  25u
#define TEST_BASE_REG_CESSB 36u
#define TEST_BASE_REG_TXF   38u
#define TEST_BASE_TOKEN     ((uint32_t)0x5458u << 16)   /* lets the base use the settings in 25, 36 and 38 */

/* What the test image starts from, and what the GUI sets at each start on it: the compressor bypassed, CESSB off,
 * the TX filter 300-3000 Hz */
#define TEST_BASE_TXF_LOW   300u
#define TEST_BASE_TXF_HIGH  3000u

typedef struct {
    uint32_t comp;      /* register 25: ratio less 2 in bits 0-3, bypass in bit 4, threshold in bits 5-10 and makeup in
                         * bits 11-16 (signed, half dB) */
    uint16_t cessb;     /* register 36's lower half: CW peak in bit 0 and its Q in bits 1-7 (Xiegu's), CESSB in bit 8,
                         * its boost in bits 9-14 (0.1 dB) */
    uint32_t    txf;    /* register 38: the low edge in Hz in bits 0-15, the high edge in bits 16-31 */
    /* To send whole once receiving: changed while transmitting, a send or a block that failed, a fault taken in.
     * Changed under the radio lock only; read anywhere (test_base_pending) */
    atomic_bool held;
    /* The base may have lost them: a failed write, a reset, a gap in its data. Set from any thread (test_base_fault),
     * taken into held under the radio lock at the start of each call below: one that comes during a send is kept for
     * the next, not lost when the send clears held */
    atomic_bool fault;
} test_base_t;

/* One register write; false: it did not get through */
typedef bool (*test_base_write_t)(void *ctx, unsigned reg, uint32_t word);

/* The base's version string (at most len bytes, not always ended) is exactly the test base's */
bool test_base_is(const char *ver, unsigned len);

void test_base_defaults(test_base_t *t);

void test_base_comp_bypass(test_base_t *t, bool bypass);
void test_base_comp_level(test_base_t *t, unsigned level);     /* the ratio less 2, 0-15 */
void test_base_comp_threshold(test_base_t *t, float db);       /* -16 to +15.5 dB, in half dB */
void test_base_comp_makeup(test_base_t *t, float db);          /* -16 to +15.5 dB, in half dB */
void test_base_cessb_on(test_base_t *t, bool on);
void test_base_cessb_boost(test_base_t *t, float db);          /* 0 to 6 dB, in 0.1 dB */
void test_base_cw_peak(test_base_t *t, bool on);
void test_base_cw_peak_q(test_base_t *t, unsigned q);          /* 0-127 */
void test_base_txf_low(test_base_t *t, unsigned hz);
void test_base_txf_high(test_base_t *t, unsigned hz);

/* Register 36 as the arming write sends it */
uint32_t test_base_armed_word(const test_base_t *t);

/* Every setting to the base, in the order above. Stops at the first write that fails, before the token: false then,
 * and the base is left with its added processing off (or as it was, when the first write failed) */
bool test_base_send(const test_base_t *t, test_base_write_t write, void *ctx);

/* A setting changed (test_base_comp_* and the others before it): all of them sent now, or held while the radio
 * transmits. A send that failed is held too */
void test_base_changed(test_base_t *t, bool transmitting, test_base_write_t write, void *ctx);
/* What was held, sent once the radio receives */
void test_base_settle(test_base_t *t, bool transmitting, test_base_write_t write, void *ctx);

/* The whole register block again (block: the library's, false when it did not get through). Words the base already
 * has change nothing there, register 36 with the token ahead of 38 included. When something is held (a fault, a
 * failed send), the base may not have them: in receive, register 36 without the token ahead of the block, and the
 * explicit send after it; if that first write fails, no block (the base may still hold its token, and register 25 in
 * the block would reach it armed): false, still held. While transmitting, the block as it is, the rest once
 * receiving. A block that fails, in any state: held */
bool test_base_resend(test_base_t *t, bool transmitting, bool (*block)(void *ctx), test_base_write_t write, void *ctx);

/* From any thread: the base may have lost the settings */
void test_base_fault(test_base_t *t);
/* From any thread: something to send once receiving (checked again under the lock) */
bool test_base_pending(test_base_t *t);
