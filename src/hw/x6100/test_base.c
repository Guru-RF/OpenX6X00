/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6100 test base "tx1a2-test" (test_base.h)
 *
 *  Copyright (c) 2026 RF.Guru
 */

#include "test_base.h"

#include <math.h>
#include <string.h>

bool test_base_is(const char *ver, unsigned len) {
    size_t n = sizeof(TEST_BASE_ID) - 1;

    if (!ver || len < n) {
        return false;
    }
    /* The string ends right after the identity: within the buffer, or at its end */
    return memcmp(ver, TEST_BASE_ID, n) == 0 && (len == n || ver[n] == '\0');
}

void test_base_defaults(test_base_t *t) {
    t->comp  = (uint32_t)1u << 4;
    t->cessb = 0;
    t->txf   = TEST_BASE_TXF_LOW | ((uint32_t)TEST_BASE_TXF_HIGH << 16);
    atomic_store(&t->held, false);
    atomic_store(&t->fault, false);
}

void test_base_fault(test_base_t *t) {
    atomic_store(&t->fault, true);
}

bool test_base_pending(test_base_t *t) {
    return atomic_load(&t->held) || atomic_load(&t->fault);
}

/* A fault that came in: held from now on */
static void take_fault(test_base_t *t) {
    if (atomic_exchange(&t->fault, false)) {
        atomic_store(&t->held, true);
    }
}

static uint32_t with_field(uint32_t word, unsigned shift, unsigned bits, uint32_t value) {
    uint32_t mask = (((uint32_t)1u << bits) - 1u) << shift;

    return (word & ~mask) | ((value << shift) & mask);
}

/* Signed half dB in 6 bits: -32 to 31 */
static uint32_t half_db(float db) {
    long v = lroundf(db * 2.0f);

    if (v < -32) v = -32;
    if (v > 31) v = 31;
    return (uint32_t)v & 0x3Fu;
}

void test_base_comp_bypass(test_base_t *t, bool bypass) {
    t->comp = with_field(t->comp, 4, 1, bypass ? 1u : 0u);
}

void test_base_comp_level(test_base_t *t, unsigned level) {
    t->comp = with_field(t->comp, 0, 4, level > 15u ? 15u : level);
}

void test_base_comp_threshold(test_base_t *t, float db) {
    t->comp = with_field(t->comp, 5, 6, half_db(db));
}

void test_base_comp_makeup(test_base_t *t, float db) {
    t->comp = with_field(t->comp, 11, 6, half_db(db));
}

void test_base_cessb_on(test_base_t *t, bool on) {
    t->cessb = (uint16_t)with_field(t->cessb, 8, 1, on ? 1u : 0u);
}

void test_base_cessb_boost(test_base_t *t, float db) {
    long v = lroundf(db * 10.0f);

    if (v < 0) v = 0;
    if (v > 60) v = 60;
    t->cessb = (uint16_t)with_field(t->cessb, 9, 6, (uint32_t)v);
}

void test_base_cw_peak(test_base_t *t, bool on) {
    t->cessb = (uint16_t)with_field(t->cessb, 0, 1, on ? 1u : 0u);
}

void test_base_cw_peak_q(test_base_t *t, unsigned q) {
    t->cessb = (uint16_t)with_field(t->cessb, 1, 7, q > 127u ? 127u : q);
}

void test_base_txf_low(test_base_t *t, unsigned hz) {
    t->txf = with_field(t->txf, 0, 16, hz > 0xFFFFu ? 0xFFFFu : hz);
}

void test_base_txf_high(test_base_t *t, unsigned hz) {
    t->txf = with_field(t->txf, 16, 16, hz > 0xFFFFu ? 0xFFFFu : hz);
}

uint32_t test_base_armed_word(const test_base_t *t) {
    return TEST_BASE_TOKEN | (uint32_t)t->cessb;
}

bool test_base_send(const test_base_t *t, test_base_write_t write, void *ctx) {
    return write(ctx, TEST_BASE_REG_CESSB, (uint32_t)t->cessb)
        && write(ctx, TEST_BASE_REG_COMP, t->comp)
        && write(ctx, TEST_BASE_REG_TXF, t->txf)
        && write(ctx, TEST_BASE_REG_CESSB, test_base_armed_word(t));
}

void test_base_changed(test_base_t *t, bool transmitting, test_base_write_t write, void *ctx) {
    take_fault(t);
    if (transmitting) {
        atomic_store(&t->held, true);
        return;
    }
    atomic_store(&t->held, !test_base_send(t, write, ctx));
}

void test_base_settle(test_base_t *t, bool transmitting, test_base_write_t write, void *ctx) {
    take_fault(t);
    if (atomic_load(&t->held) && !transmitting) {
        atomic_store(&t->held, !test_base_send(t, write, ctx));
    }
}

bool test_base_resend(test_base_t *t, bool transmitting, bool (*block)(void *ctx), test_base_write_t write, void *ctx) {
    take_fault(t);
    if (!atomic_load(&t->held) || transmitting) {
        if (block(ctx)) {
            return true;
        }
        atomic_store(&t->held, true);   /* the base may have part of it: the explicit send once receiving */
        return false;
    }
    /* The library's copy of register 36 is without the token from here, written or not; the base's is only if the
     * write got through. Not: the base may still hold the token, and the block's register 25 would reach it armed */
    if (!write(ctx, TEST_BASE_REG_CESSB, (uint32_t)t->cessb)) {
        return false;
    }
    if (!block(ctx)) {
        return false;
    }
    atomic_store(&t->held, !test_base_send(t, write, ctx));
    return true;
}
