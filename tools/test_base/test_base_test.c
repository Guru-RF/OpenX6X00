/*
 * Host test of the X6100 test base "tx1a2-test" (src/hw/x6100/test_base.c, see the Makefile):
 *  - the identity: the whole version string, nothing looser (Xiegu's V1.1.9, the ",rN" patched bases, near misses)
 *  - what the test image starts from: the compressor bypassed, CESSB off, the TX filter 300-3000 Hz
 *  - the bits of registers 25, 36 and 38, as the test image reads them, with their limits
 *  - the arming word: "TX" (0x5458) in register 36's upper half, the lower half unchanged, unsigned
 *  - the order: 36 without the token, 25, 38, 36 with the token last, for every change
 *  - a write that fails: nothing after it, no token unless 25 and 38 got through, held to try again
 *  - while transmitting: nothing written, held; sent once receiving
 *  - the whole block again: as it is, unless the base may have lost the settings (held): then, in receive only,
 *    register 36 without the token ahead of it and the explicit send after it
 */
#include "test_base.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            fails++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

/* A stand-in base: the writes as they came, and the one to fail (0: none) */
typedef struct {
    unsigned     reg[16];
    uint32_t     word[16];
    unsigned     n;
    unsigned     fail_at;   /* 1-based */
    unsigned     fault_at;  /* 1-based: a fault comes in (another thread's) as this write goes */
    test_base_t *t;
} log_t;

static bool log_write(void *ctx, unsigned reg, uint32_t word) {
    log_t *l = ctx;

    if (l->n < 16) {
        l->reg[l->n]  = reg;
        l->word[l->n] = word;
    }
    l->n++;
    if (l->fault_at == l->n && l->t) {
        test_base_fault(l->t);
    }
    return l->fail_at != l->n;
}

/* The library's whole block, in the same log: register 99 */
static bool block_ok = true;

static bool log_block(void *ctx) {
    log_write(ctx, 99, 0);
    return block_ok;
}

static void identity(void) {
    char buf[0x80];

    memset(buf, 0, sizeof(buf));
    strcpy(buf, "V1.1.9 Nov 27 2025,tx1a2-test");
    CHECK(test_base_is(buf, sizeof(buf)), "the test base, padded with NULs, is known");
    CHECK(test_base_is(TEST_BASE_ID, sizeof(TEST_BASE_ID) - 1), "the identity filling the buffer, no NUL after it");
    CHECK(!test_base_is(NULL, 0x80), "no string");
    CHECK(!test_base_is(buf, 10), "a buffer shorter than the identity");

    static const char *not_it[] = {
        "V1.1.9 Nov 27 2025,10:31:57",       /* Xiegu's own V1.1.9 */
        "V1.1.9 Nov 27 2025,tx1a2-tests",
        "V1.1.9 Nov 27 2025,tx1a2-test ",
        "V1.1.9 Nov 27 2025,tx1a2-tes",
        "V1.1.9 Nov 27 2025,tx1a3-test",
        "V1.1.8 Nov 27 2025,tx1a2-test",
        "V1.1.9 Nov 28 2025,tx1a2-test",
        "X V1.1.9 Nov 27 2025,tx1a2-test",
        " V1.1.9 Nov 27 2025,tx1a2-test",
        "V1.1.6 Oct 12 2025,r13",            /* the patched base */
        "V1.1.9 Nov 27 2025,r13",
        "V1.1.9 Nov 27 2025,tx1a2-test,r13",
        "V1.1.9",
        "",
    };
    for (size_t i = 0; i < sizeof(not_it) / sizeof(not_it[0]); i++) {
        memset(buf, 0, sizeof(buf));
        strncpy(buf, not_it[i], sizeof(buf) - 1);
        CHECK(!test_base_is(buf, sizeof(buf)), "\"%s\" is not the test base", not_it[i]);
    }
}

static void defaults(void) {
    test_base_t t;

    memset(&t, 0xA5, sizeof(t));
    test_base_defaults(&t);
    CHECK(t.comp == 0x10u, "the compressor bypassed (bit 4), all else 0: 0x%08x", t.comp);
    CHECK(t.cessb == 0, "CESSB off, no boost, CW peak off: 0x%04x", t.cessb);
    CHECK(t.txf == (300u | 3000u << 16), "the TX filter 300-3000 Hz: 0x%08x", t.txf);
    CHECK(!atomic_load(&t.held), "nothing held");
    CHECK(t.comp != 0 && t.txf != 0, "never the zeroed registers");
}

static void bits(void) {
    test_base_t t;

    test_base_defaults(&t);
    test_base_comp_bypass(&t, false);
    CHECK(t.comp == 0, "bypass off: 0x%08x", t.comp);
    test_base_comp_level(&t, 2);                /* 4:1 */
    CHECK(t.comp == 2, "the ratio less 2 in bits 0-3: 0x%08x", t.comp);
    test_base_comp_level(&t, 40);
    CHECK((t.comp & 0xFu) == 15u && (t.comp & ~0xFu) == 0, "the ratio kept to its 4 bits: 0x%08x", t.comp);
    test_base_comp_level(&t, 2);

    test_base_comp_threshold(&t, -15.5f);
    CHECK(((t.comp >> 5) & 0x3Fu) == 0x21u, "threshold -15.5 dB: -31 half dB, 6 bits: 0x%08x", t.comp);
    test_base_comp_threshold(&t, -40.0f);
    CHECK(((t.comp >> 5) & 0x3Fu) == 0x20u, "threshold below -16 dB: -16 dB: 0x%08x", t.comp);
    test_base_comp_threshold(&t, 16.0f);
    CHECK(((t.comp >> 5) & 0x3Fu) == 0x1Fu, "threshold +16 dB: +15.5 dB, not wrapped to -16: 0x%08x", t.comp);
    test_base_comp_threshold(&t, 2.5f);
    CHECK(((t.comp >> 5) & 0x3Fu) == 5u, "threshold +2.5 dB: 0x%08x", t.comp);
    test_base_comp_makeup(&t, 3.0f);
    CHECK(((t.comp >> 11) & 0x3Fu) == 6u, "makeup +3 dB in bits 11-16: 0x%08x", t.comp);
    test_base_comp_makeup(&t, -1.0f);
    CHECK(((t.comp >> 11) & 0x3Fu) == 0x3Eu, "makeup -1 dB: 0x%08x", t.comp);
    CHECK((t.comp & 0x1Fu) == 2u && ((t.comp >> 5) & 0x3Fu) == 5u, "the other fields kept: 0x%08x", t.comp);
    CHECK((t.comp >> 17) == 0, "nothing above bit 16: 0x%08x", t.comp);
    test_base_comp_bypass(&t, true);
    CHECK((t.comp & 0x10u) && (t.comp & 0xFu) == 2u, "bypass on keeps the ratio: 0x%08x", t.comp);

    test_base_cessb_on(&t, true);
    CHECK(t.cessb == 0x100u, "CESSB on: bit 8: 0x%04x", t.cessb);
    test_base_cessb_boost(&t, 3.7f);
    CHECK(((t.cessb >> 9) & 0x3Fu) == 37u, "boost 3.7 dB: 37, rounded: 0x%04x", t.cessb);
    test_base_cessb_boost(&t, 0.3f);
    CHECK(((t.cessb >> 9) & 0x3Fu) == 3u, "boost 0.3 dB: 3, not 2: 0x%04x", t.cessb);
    test_base_cessb_boost(&t, 9.0f);
    CHECK(((t.cessb >> 9) & 0x3Fu) == 60u, "boost above 6 dB: 6 dB: 0x%04x", t.cessb);
    test_base_cessb_boost(&t, -1.0f);
    CHECK(((t.cessb >> 9) & 0x3Fu) == 0u && (t.cessb & 0x100u), "no boost below 0, CESSB still on: 0x%04x", t.cessb);
    test_base_cw_peak(&t, true);
    test_base_cw_peak_q(&t, 5);
    CHECK((t.cessb & 0xFFu) == (1u | 5u << 1), "CW peak and its Q in bits 0-7: 0x%04x", t.cessb);
    CHECK(t.cessb & 0x100u, "CESSB kept: 0x%04x", t.cessb);
    CHECK((t.cessb & 0x8000u) == 0, "bit 15 never set: 0x%04x", t.cessb);

    test_base_txf_low(&t, 200);
    test_base_txf_high(&t, 2800);
    CHECK(t.txf == (200u | 2800u << 16), "the TX filter 200-2800 Hz: 0x%08x", t.txf);
    test_base_txf_high(&t, 70000);
    CHECK(t.txf == (200u | 0xFFFFu << 16), "a high edge kept to 16 bits, the low one untouched: 0x%08x", t.txf);
}

static void armed_word(void) {
    test_base_t t;

    test_base_defaults(&t);
    CHECK(test_base_armed_word(&t) == 0x54580000u, "the token alone: 0x%08x", test_base_armed_word(&t));
    test_base_cessb_on(&t, true);
    test_base_cessb_boost(&t, 6.0f);
    test_base_cw_peak(&t, true);
    test_base_cw_peak_q(&t, 127);
    uint32_t w = test_base_armed_word(&t);
    CHECK((w >> 16) == 0x5458u, "the token in the upper half: 0x%08x", w);
    CHECK((w & 0xFFFFu) == t.cessb, "the lower half as set: 0x%08x", w);
}

static void order(void) {
    test_base_t t;
    log_t       l = {0};

    test_base_defaults(&t);
    test_base_comp_bypass(&t, false);
    test_base_comp_level(&t, 1);
    test_base_cessb_on(&t, true);
    test_base_changed(&t, false, log_write, &l);

    CHECK(l.n == 4, "four writes: %u", l.n);
    CHECK(l.reg[0] == 36 && l.word[0] == t.cessb, "first: register 36 without the token (0x%08x)", l.word[0]);
    CHECK((l.word[0] >> 16) == 0, "no token in the first write");
    CHECK(l.reg[1] == 25 && l.word[1] == t.comp, "then register 25 whole (0x%08x)", l.word[1]);
    CHECK(l.reg[2] == 38 && l.word[2] == t.txf, "then register 38 whole (0x%08x)", l.word[2]);
    CHECK(l.reg[3] == 36 && l.word[3] == (0x54580000u | t.cessb), "last: register 36 with the token (0x%08x)", l.word[3]);
    CHECK(!atomic_load(&t.held), "nothing held");

    /* radio.c's ratio: bypass off, then the level, two changes: each its own whole send, off first */
    l = (log_t){0};
    test_base_comp_bypass(&t, false);
    test_base_changed(&t, false, log_write, &l);
    test_base_comp_level(&t, 4);
    test_base_changed(&t, false, log_write, &l);
    CHECK(l.n == 8, "two whole sends: %u writes", l.n);
    CHECK(l.reg[4] == 36 && (l.word[4] >> 16) == 0, "the second starts without the token too");
    CHECK(l.word[5] == t.comp && (t.comp & 0xFu) == 4u, "with the new ratio");
}

static void failures(void) {
    for (unsigned at = 1; at <= 4; at++) {
        test_base_t t;
        log_t       l = {.fail_at = at};

        test_base_defaults(&t);
        test_base_changed(&t, false, log_write, &l);
        CHECK(l.n == at, "write %u failed: nothing after it (%u writes)", at, l.n);
        CHECK(atomic_load(&t.held), "write %u failed: held, to send again", at);
        bool token = false;
        for (unsigned i = 0; i < l.n && i < 16; i++) {
            token |= (l.word[i] >> 16) == 0x5458u && !(i + 1 == at);
        }
        CHECK(!token, "write %u failed: no token got through", at);

        /* Tried again from the radio thread, in receive: whole */
        l = (log_t){0};
        test_base_settle(&t, false, log_write, &l);
        CHECK(l.n == 4 && !atomic_load(&t.held), "after write %u failed: sent again whole (%u writes)", at, l.n);
    }
}

static void transmitting(void) {
    test_base_t t;
    log_t       l = {0};

    test_base_defaults(&t);
    test_base_cessb_on(&t, true);
    test_base_changed(&t, true, log_write, &l);
    CHECK(l.n == 0, "while transmitting: nothing written (%u)", l.n);
    CHECK(atomic_load(&t.held), "held");
    test_base_txf_low(&t, 250);
    test_base_changed(&t, true, log_write, &l);
    test_base_settle(&t, true, log_write, &l);
    CHECK(l.n == 0 && atomic_load(&t.held), "still transmitting: still nothing");

    test_base_settle(&t, false, log_write, &l);
    CHECK(l.n == 4, "receiving again: one whole send (%u writes)", l.n);
    CHECK(l.word[2] == t.txf && (t.txf & 0xFFFFu) == 250u, "with every change made meanwhile");
    CHECK(l.word[3] == (0x54580000u | 0x100u), "CESSB on, armed last (0x%08x)", l.word[3]);
    CHECK(!atomic_load(&t.held), "nothing held after");

    l = (log_t){0};
    test_base_settle(&t, false, log_write, &l);
    CHECK(l.n == 0, "nothing held: nothing sent (%u)", l.n);
}

static void resend(void) {
    test_base_t t;
    log_t       l = {0};

    test_base_defaults(&t);
    test_base_cessb_on(&t, true);
    CHECK(test_base_resend(&t, false, log_block, log_write, &l), "the block got through");
    CHECK(l.n == 1 && l.reg[0] == 99, "nothing held: the block as it is (%u writes)", l.n);

    test_base_fault(&t);    /* a failed write, a reset, a gap in the data */
    l = (log_t){0};
    CHECK(test_base_resend(&t, true, log_block, log_write, &l), "in TX, the block got through");
    CHECK(l.n == 1 && l.reg[0] == 99 && atomic_load(&t.held), "while transmitting: the block as it is, still held (%u)", l.n);

    l = (log_t){0};
    CHECK(test_base_resend(&t, false, log_block, log_write, &l), "in RX, the block got through");
    CHECK(l.n == 6, "in RX: six writes (%u)", l.n);
    CHECK(l.reg[0] == 36 && l.word[0] == t.cessb, "register 36 without the token ahead of the block (0x%08x)", l.word[0]);
    CHECK(l.reg[1] == 99, "then the block");
    CHECK(l.reg[2] == 36 && (l.word[2] >> 16) == 0 && l.reg[3] == 25 && l.reg[4] == 38, "then clear, 25, 38");
    CHECK(l.reg[5] == 36 && l.word[5] == (0x54580000u | t.cessb), "and the token last (0x%08x)", l.word[5]);
    CHECK(!atomic_load(&t.held), "nothing held after");

    test_base_fault(&t);
    block_ok = false;
    l        = (log_t){0};
    CHECK(!test_base_resend(&t, false, log_block, log_write, &l), "the block did not get through");
    CHECK(l.n == 2 && atomic_load(&t.held), "the block failed: nothing after it, still held (%u)", l.n);
    block_ok = true;
}

/* The firmware's review of ed1f1d0f: a pre-clear that fails, a block that fails, a fault during a send */
static void recovery(void) {
    test_base_t t;
    log_t       l;

    /* 1. Held, in receive, the first write (register 36 without the token) fails: no block over a base that may
     *    still hold its token, false, still held */
    test_base_defaults(&t);
    test_base_cessb_on(&t, true);
    test_base_fault(&t);
    l = (log_t){.fail_at = 1};
    CHECK(!test_base_resend(&t, false, log_block, log_write, &l), "the pre-clear failed: false");
    CHECK(l.n == 1 && l.reg[0] == 36, "the pre-clear failed: no block after it (%u writes)", l.n);
    CHECK(atomic_load(&t.held) && test_base_pending(&t), "the pre-clear failed: still held");
    l = (log_t){0};
    CHECK(test_base_resend(&t, false, log_block, log_write, &l) && l.n == 6 && !test_base_pending(&t),
          "the next resend: clear, block, the explicit send (%u writes)", l.n);

    /* 2. Nothing held, the block fails, in receive and in TX: held from then on, and no clear in TX */
    l        = (log_t){0};
    block_ok = false;
    CHECK(!test_base_resend(&t, false, log_block, log_write, &l), "the steady block failed: false");
    CHECK(l.n == 1 && l.reg[0] == 99 && atomic_load(&t.held), "the steady block failed: held (%u writes)", l.n);
    test_base_defaults(&t);
    l = (log_t){0};
    CHECK(!test_base_resend(&t, true, log_block, log_write, &l), "the block failed in TX: false");
    CHECK(l.n == 1 && l.reg[0] == 99 && atomic_load(&t.held), "in TX: only the block tried, held (%u writes)", l.n);
    block_ok = true;
    l        = (log_t){0};
    test_base_settle(&t, true, log_write, &l);
    CHECK(l.n == 0 && atomic_load(&t.held), "still in TX: no clear sent (%u writes)", l.n);
    test_base_settle(&t, false, log_write, &l);
    CHECK(l.n == 4 && !test_base_pending(&t), "in receive: the explicit send (%u writes)", l.n);

    /* 3. A fault that comes while a send is under way is not lost when that send clears held */
    for (unsigned at = 1; at <= 4; at++) {
        test_base_defaults(&t);
        l = (log_t){.fault_at = at, .t = &t};
        test_base_changed(&t, false, log_write, &l);
        CHECK(l.n == 4 && !atomic_load(&t.held), "fault at write %u: the send itself went through", at);
        CHECK(test_base_pending(&t), "fault at write %u: still pending after the send", at);
        l = (log_t){0};
        test_base_settle(&t, false, log_write, &l);
        CHECK(l.n == 4 && !test_base_pending(&t), "fault at write %u: sent whole again, then nothing (%u)", at, l.n);
    }
    /* during a resend's block and its explicit send too */
    for (unsigned at = 2; at <= 6; at++) {
        test_base_defaults(&t);
        test_base_fault(&t);
        l = (log_t){.fault_at = at, .t = &t};
        CHECK(test_base_resend(&t, false, log_block, log_write, &l) && l.n == 6, "resend, fault at %u: whole", at);
        CHECK(test_base_pending(&t), "resend, fault at %u: still pending after it", at);
    }
    /* during a TX: taken in, held, nothing written */
    test_base_defaults(&t);
    test_base_fault(&t);
    l = (log_t){0};
    test_base_changed(&t, true, log_write, &l);
    CHECK(l.n == 0 && atomic_load(&t.held) && !atomic_load(&t.fault), "a fault in TX: held, nothing written");
}

/* Faults from another thread while this one sends under its lock: every fault after a send's start leads to one
 * more whole send; none is lost. The other thread: hw_flow_restart, outside the radio lock */
static test_base_t     mt;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint     faults_done;

static bool count_write(void *ctx, unsigned reg, uint32_t word) {
    (void)reg;
    (void)word;
    (*(unsigned *)ctx)++;
    return true;
}

static void *faulter(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < 20000; i++) {
        test_base_fault(&mt);
        atomic_fetch_add(&faults_done, 1);
    }
    return NULL;
}

static void threads(void) {
    pthread_t th;
    unsigned  writes = 0, sends = 0;

    test_base_defaults(&mt);
    pthread_create(&th, NULL, faulter, NULL);
    while (atomic_load(&faults_done) < 20000) {
        if (test_base_pending(&mt)) {
            pthread_mutex_lock(&lock);
            unsigned before = writes;
            test_base_settle(&mt, false, count_write, &writes);
            sends += writes != before;
            pthread_mutex_unlock(&lock);
        }
    }
    pthread_join(th, NULL);
    /* The last fault came after the last send started, or was taken by it: one more settle sends or not, then
     * nothing is pending */
    pthread_mutex_lock(&lock);
    test_base_settle(&mt, false, count_write, &writes);
    pthread_mutex_unlock(&lock);
    CHECK(!test_base_pending(&mt), "after the last fault and a settle, nothing pending");
    CHECK(writes % 4 == 0 && sends > 0, "only whole sends (%u writes, %u sends)", writes, sends);
}

int main(void) {
    identity();
    defaults();
    bits();
    armed_word();
    order();
    failures();
    transmitting();
    resend();
    recovery();
    threads();
    printf("test_base: %d of %d checks failed\n", fails, checks);
    return fails != 0;
}
