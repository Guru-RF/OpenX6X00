/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of radio.c's side of the X6200's WR12 (#134): the real src/radio.c, the settings, the base and
 *  the WR12's driver stood in (fakes.c). What reaches the base is looked at after every write, and at every wr12_want:
 *  with the VFO in use on the WR12's IF, or the WR12 out of its bypass or asked out of it, the power must be at 0 W,
 *  VOX off and KEY Train on already, nothing keyed and the base's own TX outside its band table shut (open TX at 0,
 *  #176); with the dial where TX is refused, the open TX shut or the power at 0 W (fakes.h). The open TX goes onto the
 *  WR12's IF at once, elsewhere once the dial has settled, from the radio thread, whose turn the test takes (settle).
 *  First starts of their own, each in a child (radio.c's state is the process's): the WR12 left out of its bypass by
 *  the GUI before, WR12 on HOST on and the dial on 145.5 MHz (held from the start, shut once the radio thread's turn
 *  comes, open once the WR12 is back in it), the dial outside the amateur bands (shut once settled), the library's
 *  write of the open TX not taken (opened at the first turn), a shut and an open that fail (said once, not tried at
 *  every step of the dial but 5 s later, then 10 s after one that fails again; said again after one that went through,
 *  either way; the retry not while the base is in TX), and the CW Trainer outside the bands (the base open for its
 *  sidetone, its key at once opening it first, not where the hardware cannot transmit). Then in order: WR12 on HOST off
 *  at the start, on below 54 MHz, the WR12 found; the knob to 98 MHz (held and shut before the base goes to the IF, I
 *  and Q swapped with it, PTT and the modem refused), a band key to 98.1 MHz, the knob back to 7 MHz (the base on the
 *  dial, IQ back, TX held and shut until the WR12's OK to AT+BYPASS=1, PWR, VOX and Train back as they were after it
 *  and the base open once settled, PTT then free); the VFOs swapped from A on 98 MHz to B on 7 MHz and back (held and
 *  shut before the base's VFO in use changes); WR12 on HOST off while the WR12 receives (98 MHz: shut, receive only);
 *  the WR12 unplugged while it receives (145 MHz, the knob stepped there: no write of the open TX; shut, receive only);
 *  WR12 on HOST off on 145.5 MHz (shut, receive only, not open again); Birdie Cal off the dial and back to it
 *  (radio_set_freq, radio_set_freq_dial); 54.5 MHz with TX outside the bands on, which the hardware could transmit on;
 *  a transverter over 2 m, which wins over the WR12, its IF at 29.9 MHz open too; on HF outside the amateur bands with
 *  TX outside them off (held at once, shut once settled); the knob over 14.350 MHz and back, step by step (no write at
 *  a step, one once settled); the shut and the open not written while the base is in TX by itself, an open's retry not
 *  while the GUI's PTT keys, the shut not within its CW key's hang; an open still due when the GUI keys 0.1 s after
 *  leaving 7.350 MHz (PTT on 5.3305 MHz, the modem on 475 kHz, the CW key on 136 kHz, the tuner on 40.68 MHz: the open
 *  first), an app's slot in split on 475 kHz with the dial on 7.350 MHz (open for it, shut after); the shut onto the
 *  WR12's IF at once 1 s after a shut that failed elsewhere and 1 s after the CW key on 6 m; the GUI's CW key on
 *  another thread while a write is on its way (down once it is over); the hand mic's PTT held onto the WR12's IF (the
 *  shut once it lets go); Birdie Cal on 7 MHz (held at once and shut for the whole run, on 27.185 MHz too) with the
 *  dial turned out of the bands and back meanwhile, open once it is over; TX outside the bands on (open), the switch
 *  turned off there (held at once, shut once settled); the bands of ours that the base's table has not (open). RIT the
 *  other way round on the WR12's IF, where I and Q are swapped (#134). PTT in split on the TX frequency and back, the
 *  dial not counted for it, the TX frequency refused, on VFO B, split off; what ends under it or goes on after it leaves
 *  it on the TX frequency until the last one is over, the knob, the WR12's news, the other VFO, split or a swap under it,
 *  a band key's round and a transverter's limit or end under it, the SWR scan's carrier, KEY Train's hold (#201).
 *
 *  Copyright (c) 2026 RF.Guru
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fakes.h"
#include "hw/hw.h"
#include "radio.h"
#include "wr12.h"

#define A RADIO_VFO_A
#define B RADIO_VFO_B

static int fails, checks;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    checks++;
    if (!ok) {
        fails++;
        printf("FAIL ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
}

/* Held where TX is refused: 0 W, VOX off, Train on */
static bool held(void) {
    return base.pwr == 0.0f && !base.vox && base.train;
}

/* And the base's own band table shut */
static bool shut(void) {
    return held() && base.open_tx == 0;
}

/* As the radio stood before the WR12: PWR 5 W, VOX on, KEY Train off, the base's own TX outside its band table open */
static bool as_set(void) {
    return base.pwr == 5.0f && base.vox && !base.train && base.open_tx == 1;
}

/* The VFO in use at the base, its frequency, IQ, and the WR12 asked out on hz (0: in its bypass) */
static void base_is(const char *what, radio_vfo_t cur, int32_t hz, bool iq, int32_t rx_hz) {
    check(base.cur == (int)cur && base.vfo[cur] == hz && base.iq == iq,
          "%s: the base on VFO %c %d Hz, IQ %d (VFO %c %d Hz, IQ %d)", what, "AB"[cur], (int)hz, iq, "AB"[base.cur],
          (int)base.vfo[base.cur], base.iq);
    check(want.rx == (rx_hz != 0) && (!rx_hz || want.hz == rx_hz), "%s: the WR12 asked %s %d Hz (rx %d, %d Hz)",
          what, rx_hz ? "out on" : "into its bypass", (int)rx_hz, want.rx, (int)want.hz);
}

static void none_yet(const char *what) {
    check(base.violations == 0, "%s: %d times not held where it must be, the first: %s", what, base.violations,
          base.first);
}

/* The base's open TX and how many times it was written */
static void open_is(const char *what, int open, int writes) {
    check(base.open_tx == open && base.open_tx_writes == writes, "%s: the base's open TX %d, written %d times (%d, %d)",
          what, base.open_tx, base.open_tx_writes, open, writes);
}

/* A second without a move: the dial settled (OPEN_TX_SETTLE_MS), the radio thread's turn at the open TX */
static void settle(void) {
    clock_add(1000);
    radio_thread_turn();
}

static void start(void) {
    radio_init();
    radio_start();
    gui_run();
}

/* radio.c keeps its state for the whole process: a start of another kind runs in a child, its counts back through a
 * pipe */
static void apart(const char *name, void (*run)(void)) {
    int   p[2], n[2] = {0, 0};
    pid_t pid;

    fflush(stdout);
    if (pipe(p) != 0 || (pid = fork()) < 0) {
        printf("FAIL %s: no child for it\n", name);
        fails++;
        return;
    }
    if (pid == 0) {
        close(p[0]);
        checks = fails = fake_errors = 0;   /* its own counts: the parent has the earlier children's */
        run();
        n[0] = checks;
        n[1] = fails + fake_errors;
        fflush(stdout);
        _exit(write(p[1], n, sizeof(n)) == sizeof(n) ? 0 : 1);
    }
    close(p[1]);
    if (read(p[0], n, sizeof(n)) != sizeof(n)) {
        printf("FAIL %s: the child ended without its counts\n", name);
        n[1]++;
    }
    close(p[0]);
    waitpid(pid, NULL, 0);
    checks += n[0];
    fails += n[1];
}

/* A GUI started while the WR12 may be out of its bypass (after a crash or an update), WR12 on HOST on and the dial on
 * 145.5 MHz. Held from the start; the base's open TX shut at the radio thread's first turn (not onto the IF: the
 * WR12 is not set up yet), and still while the WR12 is on its way back into its bypass; open with TX as set once it
 * is. radio_start sends the VFOs before PWR, VOX and Train (the hold is worked out first): until then the base has the
 * library's own start values, the stand-in's -1 W. Looked at from the shut on */
static void start_out(void) {
    fake_cfg_init();
    set_i(cfg_wr12, 1);
    set_i(cfg_band_vfoa_freq, 145500000);
    fake_start_out();
    start();
    check(held(), "start, maybe out: held (PWR %.1f W, VOX %d, Train %d)", base.pwr, base.vox, base.train);
    settle();
    check(shut(), "start, maybe out: held and shut (open TX %d)", base.open_tx);
    open_is("start, maybe out", 0, 1);
    base.violations = 0;
    clock_add(6000);
    said_clear();
    check(!radio_set_ptt(true) && !base.ptt, "start, maybe out: PTT refused");
    wr12_found();
    gui_run();
    base_is("start, set up", A, WR12_IF_HZ, true, 145500000);
    check(shut(), "start, set up: held and shut, open TX %d", base.open_tx);
    wr12_went_out();
    knob(7074000);
    gui_run();
    settle();
    base_is("start, then 7 MHz", A, 7074000, false, 0);
    check(shut(), "start, then 7 MHz: held and shut until the WR12's OK, open TX %d", base.open_tx);
    wr12_back_in();
    gui_run();
    settle();
    check(as_set(), "start, the WR12 in its bypass: TX as set, open TX %d", base.open_tx);
    open_is("start, the WR12 in its bypass", 1, 2);
    none_yet("start, maybe out");
}

/* The dial outside the amateur bands with TX outside them off at the start: held at once, shut once settled; open in
 * a band */
static void start_off_band(void) {
    fake_cfg_init();
    set_i(cfg_band_vfoa_freq, 7350000);
    start();
    check(held(), "7.350 MHz at the start: held");
    settle();
    check(shut(), "7.350 MHz at the start, settled: held and shut (open TX %d)", base.open_tx);
    open_is("7.350 MHz at the start", 0, 1);
    base.violations = 0;
    knob(7074000);
    gui_run();
    settle();
    check(as_set(), "7.350 MHz at the start, then 7.074 MHz: TX as set, the base open");
    open_is("7.350 MHz at the start, then 7.074 MHz", 1, 2);
    none_yet("7.350 MHz at the start");
}

/* The library's write of the open TX at the start not taken (its answer is not looked at): the first turn writes it
 * all the same, the base open */
static void start_open_tx_shut(void) {
    fake_cfg_init();
    base.open_tx = 0;
    start();
    settle();
    check(as_set(), "open TX 0 at the start: TX as set, the base open (open TX %d)", base.open_tx);
    open_is("open TX 0 at the start", 1, 1);
    knob(7350000);
    gui_run();
    settle();
    knob(7074000);
    gui_run();
    settle();
    open_is("open TX 0 at the start, 7.350 MHz and back", 1, 3);
    none_yet("open TX 0 at the start");
}

/* A shut and an open the base does not take (hw_x6200.c found the block not as it must be): said once, not tried at
 * every step of the dial, tried again 5 s later, 10 s after one that fails again, from the radio thread with no look
 * meanwhile; said again after one that went through, whichever way. Not tried while the base is in TX by itself. The
 * base keys as it was meanwhile, held at 0 W */
static void open_tx_fails(void) {
    fake_cfg_init();
    start();
    settle();
    open_is("the start", 1, 1);
    base.open_tx_fails = 2;
    said_clear();
    knob(7350000);
    gui_run();
    settle();
    open_is("a shut that fails", 1, 2);
    check(held() && said("Base's band table not set: TX held at 0 W only"),
          "a shut that fails: held at 0 W all the same, and said");
    knob(7360000);
    gui_run();
    settle();
    open_is("a shut that failed, the knob stepped: not tried again before 5 s", 1, 2);
    clock_add(4000);
    said_clear();
    radio_thread_turn();
    open_is("5 s after a shut that failed: tried again, failed again", 1, 3);
    check(!said("Base's band table not set: TX held at 0 W only"), "a shut that failed again: not said again");
    clock_add(5000);
    radio_thread_turn();
    open_is("5 s after the second shut that failed: not tried yet", 1, 3);
    clock_add(5000);
    radio_thread_turn();
    open_is("10 s after the second shut that failed: tried again", 0, 4);
    check(shut(), "the shut tried again: held and shut");
    knob(7074000);
    gui_run();
    settle();
    check(as_set(), "back in the band: TX as set, the base open");
    open_is("back in the band", 1, 5);

    /* A shut that fails, an open that goes through, a shut that fails again: said again */
    base.open_tx_fails = 1;
    said_clear();
    knob(7350000);
    gui_run();
    settle();
    open_is("a shut that fails after one that went through", 1, 6);
    check(said("Base's band table not set: TX held at 0 W only"),
          "a shut that fails after one that went through: said");
    knob(7074000);
    gui_run();
    settle();
    open_is("back in the band before its wait is over", 1, 7);
    check(as_set(), "back in the band before its wait is over: TX as set");
    base.open_tx_fails = 1;
    said_clear();
    knob(7350000);
    gui_run();
    settle();
    open_is("a shut that fails after an open that went through", 1, 8);
    check(said("Base's band table not set: TX held at 0 W only"),
          "a shut that fails after an open that went through: said again");

    /* Its retry while the base is in TX by itself (the hand mic's PTT): not before it is back in RX */
    base_keys_itself(true);
    clock_add(5000);
    radio_thread_turn();
    clock_add(60000);
    radio_thread_turn();
    open_is("the retry while the base is in TX", 1, 8);
    base_keys_itself(false);
    radio_thread_turn();
    open_is("the retry once the base is back in RX", 0, 9);
    check(shut(), "the retry once the base is back in RX: held and shut");

    /* An open that fails: said, tried again 5 s later */
    base.open_tx_fails = 1;
    said_clear();
    knob(7074000);
    gui_run();
    settle();
    open_is("an open that fails", 0, 10);
    check(said("Base still keys only inside its own band table"), "an open that fails: said");
    knob(7075000);
    gui_run();
    settle();
    open_is("an open that failed, the knob stepped", 0, 10);
    clock_add(4000);
    radio_thread_turn();
    open_is("5 s after an open that failed: tried again", 1, 11);
    check(as_set(), "an open tried again: TX as set, the base open");
}

/* KEY Train practice in the CW Trainer outside the bands (#121): the trainer's key goes through at 0 W for its
 * sidetone, which base 1.0.8 sounds by going into TX: the base open there while the trainer is open, held at 0 W, PTT
 * still refused. Not where the hardware cannot transmit (98 MHz, receive only). The trainer opened and closed is seen
 * at the radio thread's turn */
static void trainer(void) {
    fake_cfg_init();
    set_i(cfg_band_vfoa_freq, 7350000);
    start();
    settle();
    open_is("7.350 MHz", 0, 1);
    base.violations = 0;
    set_i(cfg_key_train, 1);
    set_i(cfg_band_vfoa_mode, radio_mode_cw);
    gui_run();
    settle();
    open_is("KEY Train in CW on 7.350 MHz, no trainer: still shut", 0, 1);
    fake_trainer(true);
    radio_thread_turn();
    open_is("the trainer opened on 7.350 MHz: not before the dial has settled", 0, 1);
    settle();
    check(held() && base.open_tx == 1, "the trainer on 7.350 MHz: held at 0 W, the base open (open TX %d)",
          base.open_tx);
    open_is("the trainer on 7.350 MHz", 1, 2);
    check(radio_set_morse_key(true), "the trainer on 7.350 MHz: its key goes through");
    radio_set_morse_key(false);
    fake_trainer(false);
    radio_thread_turn();
    settle();
    clock_add(3000);
    radio_thread_turn();
    open_is("the trainer closed for a moment", 0, 3);
    fake_trainer(true);
    radio_thread_turn();
    clock_add(100);
    check(radio_set_morse_key(true) && base.key && base.open_tx_at_key == 1,
          "the trainer opened again, its key at once: the base open before the key (open TX %d then)",
          base.open_tx_at_key);
    open_is("the trainer opened again, its key at once", 1, 4);
    radio_set_morse_key(false);
    clock_add(6000);
    said_clear();
    check(!radio_set_ptt(true) && !base.ptt, "the trainer on 7.350 MHz: PTT refused");
    knob(98000000);
    gui_run();
    settle();
    clock_add(3000);    /* past the key's hang */
    radio_thread_turn();
    check(shut(), "the trainer on 98 MHz: held and shut, receive only (open TX %d)", base.open_tx);
    open_is("the trainer on 98 MHz", 0, 5);
    knob(7350000);
    gui_run();
    settle();
    open_is("the trainer back on 7.350 MHz", 1, 6);
    fake_trainer(false);
    radio_thread_turn();
    settle();
    check(shut(), "the trainer closed on 7.350 MHz: held and shut (open TX %d)", base.open_tx);
    open_is("the trainer closed on 7.350 MHz", 0, 7);
    none_yet("the CW Trainer");
}

int __real_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);

/* A keyer's thread: the GUI's CW key down */
static void *key_down(void *arg) {
    (void)arg;
    radio_set_morse_key(true);
    return NULL;
}

/* The VFO in use was sent to f with PTT or the modem set, since base.n_keyed was emptied */
static bool keyed_at(int32_t f) {
    for (int k = 0; k < base.n_keyed; k++) {
        if (base.keyed_hz[k] == f) return true;
    }
    return false;
}

int main(void) {
    int writes;

    if (!freopen("radio_wr12_test.log", "w", stderr)) return 1;
    setvbuf(stderr, NULL, _IONBF, 0);
    apart("the WR12 out at the start", start_out);
    apart("7.350 MHz at the start", start_off_band);
    apart("open TX 0 at the start", start_open_tx_shut);
    apart("a shut and an open that fail", open_tx_fails);
    apart("the CW Trainer outside the bands", trainer);
    fake_cfg_init();
    start();

    /* At the start, WR12 on HOST off, VFO A on 7.074 MHz: the base on the dial, as set; its open TX written once, at
     * the radio thread's first turn (the library's write is not looked at) */
    base_is("the start", A, 7074000, false, 0);
    check(base.vfo[B] == 14074000 && as_set() && !want.on,
          "the start: VFO B on its own, PWR, VOX and Train as set, the WR12 left alone");
    settle();
    open_is("the start", 1, 1);

    /* WR12 on HOST on below 54 MHz, the WR12 found: looked for, nothing else moves */
    set_i(cfg_wr12, 1);
    gui_run();
    check(want.on && !want.rx, "on: the WR12 looked for, in its bypass");
    wr12_found();
    gui_run();
    base_is("found, 7 MHz", A, 7074000, false, 0);
    check(as_set(), "found, 7 MHz: TX as set");

    set_i(cfg_rit, 500);
    check(base.rit == 500, "RIT +500 on 7 MHz: +500 to the base (%d)", (int)base.rit);

    /* The knob to 98 MHz: held and shut before the base goes to the IF, IQ swapped, RIT the other way round with it,
     * the WR12 asked out; nothing keys */
    knob(98000000);
    gui_run();
    base_is("98 MHz", A, WR12_IF_HZ, true, 98000000);
    check(base.rit == -500, "98 MHz: RIT +500 is -500 to the base, I and Q swapped (%d)", (int)base.rit);
    set_i(cfg_rit, -300);
    check(base.rit == 300, "98 MHz: RIT -300 is +300 to the base (%d)", (int)base.rit);
    check(shut(), "98 MHz: held at 0 W, VOX off, Train on, the base shut");
    check(base.open_tx_at_vfo == 0, "98 MHz: shut before the base went to the IF");
    open_is("98 MHz", 0, 2);
    check(base.vfo[B] == 14074000, "98 MHz: VFO B on its own");
    none_yet("98 MHz");
    said_clear();
    check(!radio_set_ptt(true) && !base.ptt, "98 MHz: PTT refused");
    check(said("No TX on 98.000 MHz") && said("WR12 receiving: no TX"), "98 MHz: the refusal said");
    radio_set_modem(true);
    check(!base.modem, "98 MHz: the modem refused");
    check(radio_key_refused(NULL), "98 MHz: radio_key_refused");

    /* Out of its bypass; a band key to 98.1 MHz (the VFO itself, before the dial is told): the base stays on the IF */
    wr12_went_out();
    gui_run();
    set_i(cfg_band_vfoa_freq, 98100000);
    gui_run();
    settle();
    base_is("98.1 MHz", A, WR12_IF_HZ, true, 98100000);
    check(shut(), "98.1 MHz: held and shut");
    open_is("98.1 MHz", 0, 2);

    /* The knob back to 7.074 MHz: the base on the dial, IQ back, the WR12 asked into its bypass; held and shut until
     * it answers; then PWR, VOX and Train as set at once, the base open once the dial has settled */
    knob(7074000);
    gui_run();
    settle();
    base_is("back to 7 MHz", A, 7074000, false, 0);
    check(base.rit == -300, "back to 7 MHz: RIT -300 as set (%d)", (int)base.rit);
    set_i(cfg_rit, 0);
    check(base.rit == 0, "back to 7 MHz: RIT off");
    check(shut(), "back to 7 MHz: still held and shut, the WR12 not seen in its bypass");
    clock_add(6000);
    said_clear();
    check(!radio_set_ptt(true) && !base.ptt, "back to 7 MHz: PTT refused meanwhile");
    check(said("WR12 not in bypass: no TX"), "back to 7 MHz: the refusal said");
    wr12_back_in();
    gui_run();
    check(base.pwr == 5.0f && base.vox && !base.train && base.open_tx == 0,
          "back in its bypass: PWR %.1f W, VOX %d, Train %d as set, the base shut until settled (open TX %d)", base.pwr,
          base.vox, base.train, base.open_tx);
    settle();
    check(as_set(), "back in its bypass, settled: TX as set, the base open");
    open_is("back in its bypass", 1, 3);
    check(radio_set_ptt(true) && base.ptt, "back in its bypass: PTT goes");
    radio_set_ptt(false);
    none_yet("back to 7 MHz");

    /* The VFOs swapped: A on 98 MHz in use, B on 7.074 MHz. To B: the base on B's dial, IQ back, the WR12 into its
     * bypass, held until its OK. Back to A: held and shut before the base's VFO in use changes, then A onto the IF */
    set_i(cfg_band_vfob_freq, 7074000);
    knob(98000000);
    gui_run();
    wr12_went_out();
    gui_run();
    base_is("A on 98 MHz", A, WR12_IF_HZ, true, 98000000);
    set_i(cfg_band_current_vfo, B);
    gui_run();
    base_is("to B", B, 7074000, false, 0);
    check(base.vfo[A] == 98000000, "to B: VFO A on its own frequency, off the IF (%d)", (int)base.vfo[A]);
    check(shut(), "to B: held and shut until the WR12 is in its bypass");
    wr12_back_in();
    gui_run();
    settle();
    check(as_set(), "to B, the WR12 in its bypass: TX as set");
    set_i(cfg_band_current_vfo, A);
    gui_run();
    check(base.pwr_at_cur == 0.0f && base.open_tx_at_cur == 0,
          "back to A: held and shut before the base's VFO in use changed (PWR %.1f W, open TX %d then)",
          base.pwr_at_cur, base.open_tx_at_cur);
    base_is("back to A", A, WR12_IF_HZ, true, 98000000);
    check(shut() && base.vfo[B] == 7074000, "back to A: held and shut, B on its own");
    open_is("the VFOs swapped", 0, 6);
    none_yet("the VFOs swapped");

    /* WR12 on HOST off while the WR12 receives: the base back on the dial (98 MHz, the X6200's own FM broadcast), IQ
     * back, the WR12 into its bypass and let go; held and shut until its OK, and then as receive only */
    wr12_went_out();
    gui_run();
    set_i(cfg_wr12, 0);
    gui_run();
    base_is("off while receiving", A, 98000000, false, 0);
    check(!want.on && shut(), "off while receiving: the WR12 let go, held and shut");
    wr12_back_in();
    wr12_lost();    /* in its bypass, the port let go */
    gui_run();
    settle();
    check(shut(), "off, 98 MHz: held and shut, receive only");
    open_is("off, 98 MHz", 0, 6);
    knob(7074000);
    gui_run();
    settle();
    check(as_set(), "off, 7 MHz: TX as set");
    none_yet("off while receiving");

    /* Unplugged while it receives on 145 MHz: the base back on the dial at once, IQ back; held and shut while it may
     * still be out of its bypass, then as receive only. The knob's steps there write nothing to the base's open TX */
    set_i(cfg_wr12, 1);
    wr12_found();
    gui_run();
    knob(145000000);
    gui_run();
    base_is("145 MHz", A, WR12_IF_HZ, true, 145000000);
    writes = base.open_tx_writes;
    knob(145012500);
    knob(145025000);
    knob(145000000);
    gui_run();
    settle();
    check(base.open_tx == 0 && base.open_tx_writes == writes, "145 MHz, the knob stepped: open TX %d, written %d more",
          base.open_tx, base.open_tx_writes - writes);
    wr12_went_out();
    wr12_lost();
    gui_run();
    base_is("unplugged at 145 MHz", A, 145000000, false, 0);
    check(shut(), "unplugged at 145 MHz: held and shut");
    wr12_gone();
    gui_run();
    settle();
    check(shut() && base.open_tx_writes == writes, "unplugged for good, 145 MHz: held and shut, receive only");
    knob(7074000);
    gui_run();
    settle();
    check(as_set(), "unplugged, 7 MHz: TX as set");
    none_yet("unplugged");

    /* WR12 on HOST off while it receives on 145.5 MHz (the dial left there here; the GUI moves it to the band below,
     * receive only as well): shut through the WR12's OK and after it, not open again where TX is refused */
    wr12_found();
    gui_run();
    knob(145500000);
    gui_run();
    wr12_went_out();
    gui_run();
    writes = base.open_tx_writes;
    set_i(cfg_wr12, 0);
    gui_run();
    base_is("off at 145.5 MHz", A, 145500000, false, 0);
    wr12_back_in();
    wr12_lost();
    gui_run();
    settle();
    check(shut() && base.open_tx_writes == writes,
          "off at 145.5 MHz: held and shut, receive only (open TX %d, %d more)", base.open_tx,
          base.open_tx_writes - writes);
    clock_add(6000);
    check(!radio_set_ptt(true) && !base.ptt, "off at 145.5 MHz: PTT refused");
    knob(7074000);
    gui_run();
    settle();
    check(as_set(), "off at 145.5 MHz, then 7 MHz: TX as set");
    none_yet("off at 145.5 MHz");

    /* Birdie Cal off the dial at 98 MHz: the base on the run's frequency, IQ back, the WR12 into its bypass, held and
     * shut for the whole run; back on the dial, the base on the IF again, IQ swapped, the WR12 out again, held */
    set_i(cfg_wr12, 1);
    wr12_found();
    gui_run();
    knob(98000000);
    gui_run();
    wr12_went_out();
    gui_run();
    radio_set_freq(7100000);
    gui_run();
    base_is("Birdie Cal", A, 7100000, false, 0);
    check(shut(), "Birdie Cal: held and shut");
    wr12_back_in();
    gui_run();
    settle();
    check(shut(), "Birdie Cal, the WR12 in its bypass: still held and shut for the run");
    radio_set_freq_dial();
    gui_run();
    base_is("Birdie Cal over", A, WR12_IF_HZ, true, 98000000);
    check(shut(), "Birdie Cal over: held and shut");
    wr12_went_out();
    knob(7074000);
    wr12_back_in();
    gui_run();
    settle();
    check(as_set(), "Birdie Cal over, 7 MHz: TX as set");
    none_yet("Birdie Cal");

    /* 54.5 MHz with TX outside the bands on, where the hardware transmits: the WR12's all the same, held and shut */
    set_i(cfg_tx_outside_bands, 1);
    knob(54500000);
    gui_run();
    base_is("54.5 MHz", A, WR12_IF_HZ, true, 54500000);
    check(shut(), "54.5 MHz: held and shut, TX outside the bands on or not");
    clock_add(6000);
    said_clear();
    check(!radio_set_ptt(true) && said("WR12 receiving: no TX"), "54.5 MHz: PTT refused, the WR12's word");
    wr12_went_out();
    knob(7074000);
    wr12_back_in();
    gui_run();
    set_i(cfg_tx_outside_bands, 0);
    gui_run();
    settle();
    check(as_set(), "54.5 MHz, then 7 MHz: TX as set");
    none_yet("54.5 MHz");

    /* A transverter over 144-146 MHz with a shift: it wins, the base on its IF without the swap, the WR12 in its
     * bypass, TX as set. At 145.9 MHz its IF is 29.9 MHz, past the end of the base's 10 m: open there too, or PTT
     * would key nothing */
    set_i(cfg_transverter_0_from, 144000000);
    set_i(cfg_transverter_0_to, 146000000);
    set_i(cfg_transverter_0_shift, 116000000);
    knob(145000000);
    gui_run();
    settle();
    base_is("a transverter at 145 MHz", A, 29000000, false, 0);
    check(as_set(), "a transverter at 145 MHz: TX as set");
    knob(145900000);
    gui_run();
    settle();
    base_is("a transverter at 145.9 MHz", A, 29900000, false, 0);
    check(as_set() && radio_set_ptt(true) && base.ptt, "a transverter at 145.9 MHz: TX as set, PTT goes");
    radio_set_ptt(false);
    knob(7074000);
    gui_run();
    settle();
    none_yet("the transverter");

    /* On HF outside the amateur bands, TX outside them off: held at once, shut once the dial has settled, PTT refused;
     * back in a band, open */
    writes = base.open_tx_writes;
    knob(7350000);
    gui_run();
    check(held() && base.open_tx == 1, "7.350 MHz: held at once, the base open until the dial has settled");
    settle();
    check(shut(), "7.350 MHz, settled: held and shut (open TX %d)", base.open_tx);
    clock_add(6000);
    said_clear();
    check(!radio_set_ptt(true) && said("No TX on 7.350 MHz") && said("Out of band: Settings > TX Cal"),
          "7.350 MHz: PTT refused, said");
    knob(7074000);
    gui_run();
    settle();
    check(as_set(), "7.350 MHz, then 7.074 MHz: TX as set, the base open");
    check(base.open_tx_writes == writes + 2, "7.350 MHz and back: open TX written %d times (2)",
          base.open_tx_writes - writes);
    none_yet("7.350 MHz");

    /* The knob over 14.350 MHz and back, step by step, 0.1 s apart: held at once out of the band, nothing written at a
     * step (each would stall the knob 0.2 s), the shut once it has settled outside; the same way back */
    static const int32_t out_steps[] = {14347000, 14349000, 14351000, 14353000, 14349000, 14352000, 14355000};
    static const int32_t in_steps[]  = {14352000, 14349000, 14351000, 14347000, 14345000};

    knob(14345000);
    gui_run();
    settle();
    writes = base.open_tx_writes;
    for (size_t i = 0; i < sizeof(out_steps) / sizeof(out_steps[0]); i++) {
        knob(out_steps[i]);
        gui_run();
        clock_add(100);
        radio_thread_turn();
        check(out_steps[i] <= 14350000 || held(), "%d Hz, a step of the knob: held at once", (int)out_steps[i]);
    }
    check(base.open_tx == 1 && base.open_tx_writes == writes,
          "over 14.350 MHz step by step: open TX %d, written %d times", base.open_tx, base.open_tx_writes - writes);
    settle();
    check(shut() && base.open_tx_writes == writes + 1, "14.355 MHz, settled: held and shut, written %d times (1)",
          base.open_tx_writes - writes);
    for (size_t i = 0; i < sizeof(in_steps) / sizeof(in_steps[0]); i++) {
        knob(in_steps[i]);
        gui_run();
        clock_add(100);
        radio_thread_turn();
    }
    check(base.open_tx == 0 && base.open_tx_writes == writes + 1, "back over 14.350 MHz step by step: written %d times",
          base.open_tx_writes - writes);
    settle();
    check(as_set() && base.open_tx_writes == writes + 2, "14.345 MHz, settled: TX as set, written %d times (2)",
          base.open_tx_writes - writes);
    none_yet("over 14.350 MHz");

    /* Not while the base is in TX: by itself (the hand mic's PTT, at 0 W out of the band, and held back into it), the
     * GUI's PTT (an open's retry due meanwhile), its CW key within the hang. Each goes once it is back in RX */
    knob(14074000);
    gui_run();
    settle();
    writes = base.open_tx_writes;
    base_keys_itself(true);
    knob(14355000);
    gui_run();
    settle();
    clock_add(10000);
    radio_thread_turn();
    check(held() && base.open_tx == 1 && base.open_tx_writes == writes,
          "the hand mic's PTT out of the band: held at 0 W, the shut not written while the base is in TX");
    base_keys_itself(false);
    radio_thread_turn();
    check(shut() && base.open_tx_writes == writes + 1, "the hand mic's PTT let go: held and shut");
    base_keys_itself(true);
    knob(14074000);
    gui_run();
    settle();
    clock_add(10000);
    radio_thread_turn();
    check(base.open_tx == 0 && base.open_tx_writes == writes + 1,
          "the hand mic's PTT held back to 14.074 MHz: the open not written while the base is in TX");
    base_keys_itself(false);
    radio_thread_turn();
    check(as_set() && base.open_tx_writes == writes + 2, "the hand mic's PTT let go on 14.074 MHz: TX as set, open");
    knob(14355000);
    gui_run();
    settle();
    base.open_tx_fails = 1;
    knob(14074000);
    gui_run();
    settle();
    check(base.open_tx == 0 && base.open_tx_writes == writes + 4, "14.074 MHz, an open that fails: not open");
    check(radio_set_ptt(true) && base.ptt && base.open_tx_writes == writes + 4,
          "14.074 MHz, PTT 1 s after an open that failed: goes, the open not tried again before its wait is over");
    clock_add(10000);
    radio_thread_turn();
    check(base.open_tx == 0 && base.open_tx_writes == writes + 4, "14.074 MHz, PTT on past the wait: not tried in TX");
    radio_set_ptt(false);
    radio_thread_turn();
    check(as_set() && base.open_tx_writes == writes + 5, "14.074 MHz, PTT off: tried again, TX as set, the base open");
    check(radio_set_morse_key(true), "14.074 MHz: the CW key goes");
    radio_set_morse_key(false);
    knob(14355000);
    gui_run();
    settle();
    check(base.open_tx == 1 && base.open_tx_writes == writes + 5, "14.355 MHz within the CW key's hang: not written");
    clock_add(2500);
    radio_thread_turn();
    check(shut() && base.open_tx_writes == writes + 6, "14.355 MHz after the CW key's hang: held and shut");
    knob(14074000);
    gui_run();
    settle();
    check(as_set() && base.open_tx_writes == writes + 7, "14.074 MHz again: TX as set");
    none_yet("in TX");

    /* An open still due when the GUI keys goes first, before the key, on a band of ours the base's table has not, 0.1 s
     * after the knob left 7.350 MHz: PTT on 5.3305 MHz (60 m's first channel in the US), the modem (an app's slot) on
     * 475 kHz, the CW key on 136 kHz, the tuner on 40.68 MHz. Nothing written during the TX after */
    static const int32_t keyed_on[] = {5330500, 475000, 136000, 40680000};

    for (size_t i = 0; i < sizeof(keyed_on) / sizeof(keyed_on[0]); i++) {
        int32_t f = keyed_on[i];

        knob(7350000);
        gui_run();
        settle();
        clock_add(3000);    /* past the CW key's hang */
        radio_thread_turn();
        check(shut(), "%d Hz: held and shut on 7.350 MHz before (open TX %d)", (int)f, base.open_tx);
        writes = base.open_tx_writes;
        knob(f);
        gui_run();
        clock_add(100);
        base.open_tx_at_key = -1;
        switch (i) {
            case 0:  check(radio_set_ptt(true) && base.ptt, "%d Hz, at once: PTT goes", (int)f); break;
            case 1:  radio_set_modem(true); check(base.modem, "%d Hz, at once: the modem goes", (int)f); break;
            case 2:  check(radio_set_morse_key(true) && base.key, "%d Hz, at once: the CW key goes", (int)f); break;
            default: radio_start_atu(); break;
        }
        check(base.open_tx == 1 && base.open_tx_writes == writes + 1 && (i == 3 || base.open_tx_at_key == 1),
              "%d Hz, keyed at once: the base open before the key (open TX %d then, written %d times)", (int)f,
              base.open_tx_at_key, base.open_tx_writes - writes);
        settle();
        settle();
        check(base.open_tx_writes == writes + 1, "%d Hz, keyed: nothing written meanwhile", (int)f);
        radio_set_ptt(false);
        radio_set_modem(false);
        radio_set_morse_key(false);
        base_keys_itself(false);    /* the tuner given up: its state back to RX, as no packet comes here */
        gui_run();
    }
    knob(7074000);
    gui_run();
    clock_add(3000);
    settle();
    check(as_set(), "the keys at once, then 7.074 MHz: TX as set");
    none_yet("the keys at once");

    /* In split, the dial on 7.350 MHz, where TX is refused: the base shut for it. An app's slot on VFO B's 475 kHz,
     * which the base's table has not: open before the modem keys (radio_set_tx_freq moves the VFO in use there), shut
     * again once the slot is over and the dial is back. The stand-in's look at the dial knows no split: what it counts
     * in the slot is not counted */
    knob(7350000);
    set_i(cfg_band_vfob_freq, 475000);
    set_i(cfg_band_split, 1);
    gui_run();
    settle();
    check(shut(), "split, the dial on 7.350 MHz: held and shut (open TX %d)", base.open_tx);
    writes = base.open_tx_writes;
    base.open_tx_at_key = -1;
    radio_set_modem(true);
    check(base.modem && base.vfo[A] == 475000 && base.open_tx_at_key == 1 && base.open_tx_writes == writes + 1,
          "split, an app's slot on 475 kHz: the base open before the modem keys (open TX %d then, on %d Hz)",
          base.open_tx_at_key, (int)base.vfo[A]);
    settle();
    check(base.open_tx_writes == writes + 1, "split, the slot on: nothing written meanwhile");
    radio_set_modem(false);
    gui_run();
    base.violations = 0;
    settle();
    check(shut() && base.vfo[A] == 7350000 && base.open_tx_writes == writes + 2,
          "split, the slot over: the dial back, held and shut (open TX %d)", base.open_tx);

    /* PTT in split (#201): the base keys the VFO in use whatever split says, so that VFO goes to the TX frequency for
     * the key, open first, and back after. The dial refused does not count for it, it does for what the base keys by
     * itself and the GUI's CW key */
    int32_t refused_at = 0;

    check(radio_key_refused(&refused_at) && refused_at == 7350000 && !radio_ptt_refused(NULL),
          "split, the dial on 7.350 MHz: the key refused there (%d), PTT not", (int)refused_at);
    check(radio_key_freq() == 7350000, "split, not keyed: the base would key the dial (%d)", (int)radio_key_freq());
    base.open_tx_at_key = -1;
    int keyed = base.vfo_writes_keyed;

    check(base.split, "split: the base's bit on");
    check(radio_set_ptt(true) && base.ptt && base.vfo[A] == 475000 && base.open_tx_at_key == 1,
          "split, PTT: on B's 475 kHz, the base open before it keys (open TX %d then, on %d Hz)", base.open_tx_at_key,
          (int)base.vfo[A]);
    check(base.pwr_at_ptt == 5.0f && !base.split, "split, PTT: at PWR (%.1f W), the bit off", base.pwr_at_ptt);
    check(base.vfo_writes_keyed == keyed, "split, PTT: the VFO moved before the key went down");
    check(radio_key_freq() == 475000, "split, PTT: it keys 475 kHz (%d)", (int)radio_key_freq());
    radio_set_ptt(false);
    check(!base.ptt && base.pwr == 0.0f && base.vfo[A] == 7350000 && base.split,
          "split, PTT off: 0 W and the dial back at once, the bit on");
    check(base.vfo_writes_keyed == keyed, "split, PTT off: the VFO moved after the key went up");
    gui_run();
    base.violations = 0;
    settle();
    check(!base.ptt && shut() && base.vfo[A] == 7350000, "split, PTT off: the dial back, held and shut");

    /* The TX frequency refused: nothing moves */
    knob(7100000);
    set_i(cfg_band_vfob_freq, 7350000);
    gui_run();
    settle();
    clock_add(6000);
    said_clear();
    check(radio_ptt_refused(&refused_at) && refused_at == 7350000, "split, B on 7.350 MHz: PTT refused there");
    check(!radio_set_ptt(true) && !base.ptt && base.vfo[A] == 7100000 && said("No TX on 7.350 MHz"),
          "split, B on 7.350 MHz: PTT refused, said, nothing moved (%d)", (int)base.vfo[A]);
    radio_set_ptt(false);
    check(base.vfo[A] == 7100000, "split, PTT off with nothing moved: as it was");

    /* Both free. A second PTT does not move again; the modem on and off under it leaves it there */
    set_i(cfg_band_vfob_freq, 7150000);
    gui_run();
    settle();
    check(radio_set_ptt(true) && base.ptt && base.vfo[A] == 7150000, "split, PTT: on B's 7.150 MHz (%d)", (int)base.vfo[A]);
    int moves = base.vfo_writes;

    check(radio_set_ptt(true) && base.vfo_writes == moves, "split, PTT again (CAT): no move");
    radio_set_modem(true);
    radio_set_modem(false);
    check(base.ptt && base.vfo[A] == 7150000 && !base.split, "split, the modem on and off under PTT: still there, keyed");
    radio_set_ptt(false);
    check(!base.ptt && base.vfo[A] == 7100000 && base.split, "split, PTT off: A back on 7.100 MHz (%d)", (int)base.vfo[A]);
    moves = base.vfo_writes;
    radio_set_ptt(false);
    check(base.vfo_writes == moves, "split, PTT off again: nothing moves");

    /* A voice message on the air, PTT pressed to answer (its worker stops after the key): the modem ends under PTT, which
     * stays on the TX frequency until it lets go */
    radio_set_modem(true);
    check(base.modem && base.vfo[A] == 7150000, "split, the modem: on 7.150 MHz");
    check(radio_set_ptt(true) && base.ptt, "split, PTT under the modem: keyed");
    radio_set_modem(false);
    check(base.ptt && base.vfo[A] == 7150000 && !base.split && radio_key_freq() == 7150000,
          "split, the modem over under PTT: still on 7.150 MHz (%d)", (int)base.vfo[A]);
    radio_set_ptt(false);
    check(base.vfo[A] == 7100000 && base.split, "split, PTT off after: A back on 7.100 MHz (%d)", (int)base.vfo[A]);

    /* An app's slot the same (radio_set_tx_freq before it keys and at its end), a tone off the TX frequency */
    radio_set_tx_freq(7151000);
    radio_set_modem(true);
    check(radio_set_ptt(true) && base.vfo[A] == 7151000, "split, PTT under an app's slot: on its tone");
    radio_set_modem(false);
    radio_set_tx_freq(0);
    check(base.ptt && base.vfo[A] == 7150000 && !base.split, "split, the slot over under PTT: on 7.150 MHz (%d)",
          (int)base.vfo[A]);
    radio_set_ptt(false);
    check(base.vfo[A] == 7100000 && base.split, "split, PTT off after the slot: A back");

    /* The other way round: PTT lets go first, the audio goes on there and its end takes the VFO back */
    check(radio_set_ptt(true) && base.vfo[A] == 7150000, "split, PTT first");
    radio_set_modem(true);
    radio_set_ptt(false);
    check(!base.ptt && base.modem && base.vfo[A] == 7150000 && !base.split, "split, PTT off under the modem: goes on there");
    radio_set_modem(false);
    check(base.vfo[A] == 7100000 && base.split, "split, the modem off: A back on 7.100 MHz (%d)", (int)base.vfo[A]);

    /* The SWR window closed under PTT (radio_set_tx_freq(0) with nothing of its own moved) */
    check(radio_set_ptt(true), "split, PTT");
    radio_set_tx_freq(0);
    check(base.ptt && base.vfo[A] == 7150000 && !base.split, "split, a window's end under PTT: on 7.150 MHz (%d)",
          (int)base.vfo[A]);

    /* The knob under PTT: the dial moves, the transmission does not, it goes to the base at the release */
    knob(7100100);
    gui_run();
    check(base.ptt && base.vfo[A] == 7150000 && base.pwr == 5.0f && radio_key_freq() == 7150000,
          "split, the knob under PTT: still on 7.150 MHz at PWR (%d)", (int)base.vfo[A]);
    radio_set_ptt(false);
    check(base.vfo[A] == 7100100, "split, PTT off: A on the dial it was turned to (%d)", (int)base.vfo[A]);
    knob(7100000);
    gui_run();
    settle();

    /* The other VFO, the TX frequency, written under PTT (CAT): the transmission follows; onto a refused one held at
     * once and stopped */
    check(radio_set_ptt(true) && base.vfo[A] == 7150000, "split, PTT");
    set_i(cfg_band_vfob_freq, 7160000);
    gui_run();
    check(base.ptt && base.vfo[A] == 7160000 && base.vfo[B] == 7160000 && radio_key_freq() == 7160000,
          "split, B to 7.160 MHz under PTT: it goes there (%d)", (int)base.vfo[A]);
    said_clear();
    set_i(cfg_band_vfob_freq, 7350000);
    check(base.pwr == 0.0f, "split, B to 7.350 MHz under PTT: 0 W at once (%.1f W)", base.pwr);
    gui_run();
    settle();
    gui_run();
    check(!base.ptt && base.vfo[A] == 7100000 && said("TX stopped on 7.350 MHz"), "split, B refused under PTT: stopped, A back");
    set_i(cfg_band_vfob_freq, 7150000);
    gui_run();
    base.violations = 0;
    settle();

    /* Split off under PTT: on the dial, what the radio transmits on now; nothing left over at the release */
    check(radio_set_ptt(true) && base.vfo[A] == 7150000, "split, PTT");
    set_i(cfg_band_split, 0);
    gui_run();
    check(base.ptt && base.vfo[A] == 7100000 && radio_key_freq() == 7100000, "split off under PTT: on the dial (%d)",
          (int)base.vfo[A]);
    radio_set_ptt(false);
    check(!base.ptt && base.vfo[A] == 7100000 && !base.split, "split off, PTT off: on the dial, the bit off");
    set_i(cfg_band_split, 1);
    gui_run();
    settle();

    /* The VFOs swapped under PTT: the VFO in use is B now, on A's frequency, the one it transmits on now */
    check(radio_set_ptt(true) && base.vfo[A] == 7150000, "split, PTT");
    set_i(cfg_band_current_vfo, B);
    gui_run();
    check(base.ptt && base.cur == B && base.vfo[B] == 7100000 && radio_key_freq() == 7100000,
          "split, swapped under PTT: B on A's 7.100 MHz (%d)", (int)base.vfo[B]);
    radio_set_ptt(false);
    check(base.vfo[A] == 7100000 && base.vfo[B] == 7150000 && base.split, "split, swapped, PTT off: both on their own");
    set_i(cfg_band_current_vfo, A);
    gui_run();
    settle();

    /* The WR12's news under PTT (it goes through the VFOs again): the transmission stays. Lost, found again below */
    check(radio_set_ptt(true) && base.vfo[A] == 7150000, "split, PTT");
    wr12_lost();
    gui_run();
    check(base.ptt && base.vfo[A] == 7150000 && base.pwr == 5.0f, "split, the WR12 lost under PTT: still on 7.150 MHz at PWR (%d)",
          (int)base.vfo[A]);
    radio_set_ptt(false);
    check(base.vfo[A] == 7100000, "split, PTT off: A back on 7.100 MHz (%d)", (int)base.vfo[A]);
    wr12_gone();
    wr12_found();
    gui_run();
    settle();

    /* A band key's round under PTT: the VFO in use hears first, the other one's frequency already new, and the dial is
     * not (so no observer of the dial runs before it). Refused: it does not go there, 0 W before anything else, stopped
     * once looked at */
    check(radio_set_ptt(true) && base.vfo[A] == 7150000, "split, PTT");
    keyed = base.vfo_writes_keyed;
    set_quiet(cfg_band_vfob_freq, 7350000);
    notify_now(cfg_band_vfoa_freq);
    check(base.ptt && base.vfo[A] == 7150000 && base.pwr == 0.0f && base.vfo_writes_keyed == keyed,
          "a band key's round to a refused B under PTT: not gone there, 0 W (%d, %.1f W)", (int)base.vfo[A], base.pwr);
    notify_now(cfg_band_vfob_freq);
    gui_run();
    settle();
    gui_run();
    check(!base.ptt && base.vfo[A] == 7100000, "a band key's round to a refused B under PTT: stopped, A on its dial");
    set_i(cfg_band_vfob_freq, 7150000);
    knob(7100000);
    gui_run();
    base.violations = 0;
    settle();

    /* The same onto a transverter with a limit: the limit before the write. Then the transverter off under PTT: not gone
     * to 145.5 MHz, which only receives then */
    set_f(cfg_transverter_0_pwr, 1.0f);
    set_i(cfg_transverter_0_from, 144000000);
    set_i(cfg_transverter_0_to, 146000000);
    set_i(cfg_transverter_0_shift, 116000000);
    gui_run();
    settle();
    check(radio_set_ptt(true) && base.vfo[A] == 7150000 && base.pwr_at_ptt == 5.0f, "split, PTT at PWR");
    set_quiet(cfg_band_vfob_freq, 145500000);
    notify_now(cfg_band_vfoa_freq);
    check(base.vfo[A] == 29500000 && base.pwr_at_keyed_vfo == 1.0f, "a band key's round onto a 1 W transverter: 1 W before it goes (%.1f W, on %d)",
          base.pwr_at_keyed_vfo, (int)base.vfo[A]);
    notify_now(cfg_band_vfob_freq);
    gui_run();
    set_i(cfg_transverter_0_shift, 0);
    check(base.vfo[A] == 29500000 && base.pwr == 0.0f,
          "the transverter off under PTT: not gone to 145.5 MHz, 0 W (%d, %.1f W)", (int)base.vfo[A], base.pwr);
    gui_run();
    settle();
    gui_run();
    check(!base.ptt, "the transverter off under PTT: stopped");
    radio_set_ptt(false);
    set_i(cfg_band_vfob_freq, 7150000);
    set_f(cfg_transverter_0_pwr, 0.0f);
    set_i(cfg_transverter_0_from, 0);
    set_i(cfg_transverter_0_to, 0);
    knob(7100000);
    gui_run();
    base.violations = 0;
    settle();
    check(as_set(), "the transverter gone: TX as set (%.1f W)", base.pwr);

    /* A transverter with a limit turned on under PTT, over the TX frequency: its observer goes through the VFOs before the
     * limit's own does, so the limit looks at the TX frequency first, not at the dial */
    set_i(cfg_band_vfob_freq, 50100000);
    set_f(cfg_transverter_0_pwr, 1.0f);
    set_i(cfg_transverter_0_from, 50000000);
    set_i(cfg_transverter_0_to, 52000000);
    gui_run();
    settle();
    check(radio_set_ptt(true) && base.vfo[A] == 50100000 && base.pwr_at_ptt == 5.0f, "split, PTT on 50.1 MHz at PWR (%d)", (int)base.vfo[A]);
    set_i(cfg_transverter_0_shift, 22000000);
    check(base.vfo[A] == 28100000 && base.pwr_at_keyed_vfo == 1.0f, "a 1 W transverter on under PTT: 1 W before it goes (%.1f W, on %d)",
          base.pwr_at_keyed_vfo, (int)base.vfo[A]);
    radio_set_ptt(false);
    set_i(cfg_transverter_0_shift, 0);
    set_i(cfg_band_vfob_freq, 7150000);
    set_f(cfg_transverter_0_pwr, 0.0f);
    set_i(cfg_transverter_0_from, 0);
    set_i(cfg_transverter_0_to, 0);
    gui_run();
    base.violations = 0;
    settle();
    check(as_set() && base.vfo[A] == 7100000, "the transverter gone: TX as set, A back (%.1f W)", base.pwr);

    /* PTT on and off during the SWR scan's carrier, which is keyed without the modem's state: the sweep goes on where it
     * was, the window takes the VFO back */
    check(radio_start_swrscan(7140000) && base.vfo[A] == 7140000, "split, the SWR scan on 7.140 MHz (%d)", (int)base.vfo[A]);
    check(radio_set_ptt(true) && base.vfo[A] == 7140000, "split, PTT under the scan: stays on its point");
    radio_set_ptt(false);
    check(base.modem && base.vfo[A] == 7140000, "split, PTT off under the scan: the carrier stays on its point (%d)", (int)base.vfo[A]);
    radio_stop_swrscan();
    radio_set_tx_freq(0);
    gui_run();
    base.violations = 0;
    settle();
    check(!base.modem && base.vfo[A] == 7100000 && base.split, "split, the scan over: A back on 7.100 MHz (%d)", (int)base.vfo[A]);

    /* KEY Train: B in CW holds the power at 0 W in split, PTT or not */
    set_i(cfg_band_vfob_mode, radio_mode_cw);
    set_i(cfg_key_train, 1);
    gui_run();
    settle();
    check(base.pwr == 0.0f, "split, KEY Train, B in CW: 0 W (%.1f W)", base.pwr);
    check(radio_set_ptt(true) && base.pwr == 0.0f && base.pwr_at_ptt == 0.0f, "split, KEY Train, PTT: still 0 W (%.1f W)",
          base.pwr);
    radio_set_ptt(false);
    check(base.pwr == 0.0f && base.vfo[A] == 7100000, "split, KEY Train, PTT off: 0 W, A back");

    /* A voice message goes at PWR (the other VFO's CW holds nothing for it); PTT breaking in holds, nothing moved or not */
    radio_set_modem(true);
    check(base.modem && base.pwr == 5.0f, "split, KEY Train, the modem: at PWR (%.1f W)", base.pwr);
    check(radio_set_ptt(true) && base.pwr == 0.0f, "split, KEY Train, PTT under the modem: 0 W (%.1f W)", base.pwr);
    radio_set_modem(false);
    check(base.ptt && base.pwr == 0.0f, "split, KEY Train, the modem over under PTT: 0 W (%.1f W)", base.pwr);
    radio_set_ptt(false);
    check(base.pwr == 0.0f && base.vfo[A] == 7100000, "split, KEY Train, PTT off after: 0 W, A back");

    /* PTT first, the audio under it, PTT lets go: the audio at PWR, as it goes by itself */
    check(radio_set_ptt(true) && base.pwr == 0.0f, "split, KEY Train, PTT: 0 W");
    radio_set_modem(true);
    radio_set_ptt(false);
    check(base.modem && base.pwr == 5.0f, "split, KEY Train, PTT off under the modem: at PWR (%.1f W)", base.pwr);
    radio_set_modem(false);
    check(base.pwr == 0.0f && base.vfo[A] == 7100000, "split, KEY Train, the modem off: 0 W, A back");
    set_i(cfg_key_train, 0);
    set_i(cfg_band_vfob_mode, radio_mode_usb);
    gui_run();
    settle();
    check(as_set(), "split, KEY Train off: TX as set");

    /* What ends under PTT does not send the VFO in use where TX is refused: a voice message, CAT's PTT under it, B
     * then refused; the GUI's stop (the modem off first), or the message over by itself before it. A stays at 0 W
     * and goes back to the dial with PTT */
    for (int over = 0; over < 2; over++) {
        base.n_keyed = 0;
        radio_set_modem(true);
        check(base.modem && base.vfo[A] == 7150000, "split, the modem: on 7.150 (%d)", base.vfo[A]);
        check(radio_set_ptt(true) && base.ptt, "split, PTT under the modem");
        set_i(cfg_band_vfob_freq, 7350000);
        check(base.vfo[A] == 7150000 && base.pwr == 0.0f, "split, PTT under the modem, B refused: A stays at 0 W (%d, %.1f W)",
              base.vfo[A], base.pwr);
        if (over) {
            radio_set_modem(false);
            check(!keyed_at(7350000), "split, PTT, B refused, the modem over: A not sent there keyed");
        }
        gui_run();
        settle();
        gui_run();
        check(!keyed_at(7350000), "split, PTT under the modem, B refused, the stop: A not sent there keyed");
        check(!base.ptt && !base.modem && base.vfo[A] == 7100000, "split, PTT under the modem, B refused: stopped, A back");
        set_i(cfg_band_vfob_freq, 7150000);
        gui_run();
        settle();
    }

    /* PTT under the SWR scan (CAT, the PTT key) goes with the scan's stop, and not where TX is refused: TX outside the
     * bands switched off with B outside them, or B to a refused frequency */
    for (int how = 0; how < 2; how++) {
        if (!how) {
            set_i(cfg_tx_outside_bands, 1);
            set_i(cfg_band_vfob_freq, 7350000);
            gui_run();
            settle();
        }
        base.n_keyed = 0;
        int32_t from = how ? 7140000 : 7340000;
        check(radio_start_swrscan(from) && base.vfo[A] == from, "split, the SWR scan from %d (%d)", from, base.vfo[A]);
        check(radio_set_ptt(true) && base.ptt, "split, PTT under the SWR scan");
        swrscan_stop_runs = true;
        if (how) {
            set_i(cfg_band_vfob_freq, 7350000);
        } else {
            set_i(cfg_tx_outside_bands, 0);
        }
        gui_run();
        settle();
        gui_run();
        clock_add(60000);
        radio_thread_turn();
        gui_run();
        check(!base.ptt && radio_get_state() == RADIO_RX, how ? "split, PTT under the scan, B refused: PTT let go" :
              "split, PTT under the scan, TX outside the bands off: PTT let go");
        check(!keyed_at(7350000), "split, PTT under the scan, refused: A not sent to 7.350 keyed");
        swrscan_stop_runs = false;
        radio_set_ptt(false);
        set_i(cfg_band_vfob_freq, 7150000);
        gui_run();
        settle();
        check(base.vfo[A] == 7100000 && as_set(), "split, after the scan's stop: A on the dial, TX as set");
    }

    /* VFO B in use: the same the other way round */
    set_i(cfg_band_current_vfo, B);
    gui_run();
    settle();
    check(radio_key_freq() == 7150000, "split on B, not keyed: the base would key B (%d)", (int)radio_key_freq());
    check(radio_set_ptt(true) && base.vfo[B] == 7100000, "split on B, PTT: on A's 7.100 MHz (%d)", (int)base.vfo[B]);
    check(radio_key_freq() == 7100000, "split on B, PTT: it keys 7.100 MHz (%d)", (int)radio_key_freq());
    radio_set_ptt(false);
    check(base.vfo[B] == 7150000, "split on B, PTT off: B back on 7.150 MHz (%d)", (int)base.vfo[B]);
    set_i(cfg_band_current_vfo, A);
    gui_run();
    settle();

    /* Split off: PTT where it is */
    set_i(cfg_band_split, 0);
    gui_run();
    settle();
    check(radio_set_ptt(true) && base.vfo[A] == 7100000 && radio_key_freq() == 7100000, "no split, PTT: on the dial");
    radio_set_ptt(false);
    check(base.vfo[A] == 7100000, "no split, PTT off: on the dial");
    knob(7074000);
    set_i(cfg_band_vfob_freq, 14074000);
    gui_run();
    settle();
    check(as_set(), "split off, 7.074 MHz: TX as set");
    none_yet("split");

    /* Onto the WR12's IF the shut goes at once, before the base goes there: 1 s after a shut that failed on 7.350 MHz
     * too (its wait was for that one), and 1 s after the GUI's CW key on 6 m (its hang is for the GUI's CW, which does
     * not key on the IF) */
    knob(7350000);
    gui_run();
    base.open_tx_fails = 1;
    settle();
    check(base.open_tx == 1 && held(), "7.350 MHz, a shut that fails: held at 0 W, open");
    knob(98000000);
    gui_run();
    check(shut() && base.open_tx_at_vfo == 0 && base.vfo[A] == WR12_IF_HZ,
          "98 MHz 1 s after a shut that failed: shut before the base went to the IF (open TX %d then)",
          base.open_tx_at_vfo);
    wr12_went_out();
    knob(7074000);
    wr12_back_in();
    gui_run();
    settle();
    check(as_set(), "98 MHz, then 7.074 MHz: TX as set");
    knob(50100000);
    gui_run();
    settle();
    check(radio_set_morse_key(true), "50.1 MHz: the CW key goes");
    clock_add(100);
    radio_set_morse_key(false);
    clock_add(1000);
    knob(432100000);
    gui_run();
    check(shut() && base.open_tx_at_vfo == 0 && base.vfo[A] == WR12_IF_HZ,
          "432.1 MHz 1 s after the CW key on 6 m: shut before the base went to the IF (open TX %d then)",
          base.open_tx_at_vfo);
    wr12_went_out();
    knob(7074000);
    wr12_back_in();
    gui_run();
    clock_add(3000);
    settle();
    check(as_set(), "432.1 MHz, then 7.074 MHz: TX as set");
    none_yet("onto the IF at once");

    /* The GUI's CW key while a write of the open TX is on its way on another thread: its pin is not under the radio
     * lock, it goes down only once the write is over */
    pthread_t keyer;

    open_tx_write_begin();
    if (__real_pthread_create(&keyer, NULL, key_down, NULL) != 0) {
        open_tx_write_end();
        check(false, "the CW key in a write: no thread for it");
    } else {
        usleep(50000);
        check(!atomic_load(&base.key), "the CW key while a write is on its way: not down yet");
        open_tx_write_end();
        pthread_join(keyer, NULL);
        check(atomic_load(&base.key), "the CW key once the write is over: down");
    }
    radio_set_morse_key(false);
    clock_add(3000);

    /* The hand mic's PTT held onto the WR12's IF: held at 0 W before the base goes there, the shut once it lets go,
     * not later. The base open on the IF meanwhile is the case's, not a violation */
    int seen = base.violations;

    writes = base.open_tx_writes;
    base_keys_itself(true);
    knob(98000000);     /* 2 m is the transverter's now */
    gui_run();
    check(held() && base.open_tx == 1 && base.vfo[A] == WR12_IF_HZ,
          "the hand mic's PTT held onto the WR12's IF: held at 0 W at once, the shut not written in TX (open TX %d)",
          base.open_tx);
    base.violations = seen;
    base_keys_itself(false);
    radio_thread_turn();
    check(shut() && base.open_tx_writes == writes + 1, "the hand mic's PTT let go on the WR12's IF: held and shut");
    wr12_went_out();
    knob(7074000);
    wr12_back_in();
    gui_run();
    settle();
    check(as_set(), "the WR12's IF, then 7 MHz: TX as set");
    none_yet("the hand mic onto the WR12's IF");

    /* Birdie Cal from 7.074 MHz: held at 0 W at once, before the base goes off the dial, shut once settled, for the
     * whole run, and on 27.185 MHz (the CB bands of the band list), whatever the dial allows. The dial turned out of
     * the bands and back meanwhile: still held and shut. Over: TX as set, the base open; written twice for the run */
    writes = base.open_tx_writes;
    radio_set_freq(7100000);
    gui_run();
    check(held() && base.vfo[A] == 7100000, "Birdie Cal on 7 MHz: held at once");
    settle();
    check(shut(), "Birdie Cal on 7 MHz, settled: held and shut (open TX %d)", base.open_tx);
    radio_set_freq(27185000);
    gui_run();
    settle();
    check(shut() && base.vfo[A] == 27185000, "Birdie Cal on 27.185 MHz: held and shut (open TX %d)", base.open_tx);
    knob(7350000);
    gui_run();
    settle();
    check(shut(), "Birdie Cal, the dial to 7.350 MHz: held and shut (open TX %d)", base.open_tx);
    knob(7074000);
    gui_run();
    settle();
    check(shut(), "Birdie Cal, the dial back to 7.074 MHz: still held and shut for the run (open TX %d)",
          base.open_tx);
    radio_set_freq_dial();
    gui_run();
    settle();
    check(as_set(), "Birdie Cal over on 7 MHz: TX as set, the base open");
    check(base.open_tx_writes == writes + 2, "Birdie Cal on 7 MHz: open TX written %d times (2)",
          base.open_tx_writes - writes);
    none_yet("Birdie Cal on 7 MHz");

    /* TX outside them on: open there, PTT goes; the switch off while there: held at once, shut once settled */
    set_i(cfg_tx_outside_bands, 1);
    gui_run();
    knob(7350000);
    gui_run();
    settle();
    check(as_set() && radio_set_ptt(true) && base.ptt, "7.350 MHz, TX outside the bands on: TX as set, PTT goes");
    radio_set_ptt(false);
    set_i(cfg_tx_outside_bands, 0);
    check(held(), "7.350 MHz, the switch off: held at once");
    settle();
    check(shut(), "7.350 MHz, the switch off, settled: held and shut (open TX %d)", base.open_tx);
    knob(7074000);
    gui_run();
    settle();
    check(as_set(), "the switch off, then 7.074 MHz: TX as set");
    none_yet("TX outside the bands on");

    /* Ours but not in the base's table: 2200 m, 630 m, the Alaska channel, 60 m below 5.332 MHz and above 5.405 MHz,
     * 8 m. Open there */
    static const int32_t ours[] = {136000, 475000, 5167500, 5300000, 5406500, 40680000};

    writes = base.open_tx_writes;
    for (size_t i = 0; i < sizeof(ours) / sizeof(ours[0]); i++) {
        knob(ours[i]);
        gui_run();
        settle();
        check(as_set() && radio_set_ptt(true) && base.ptt, "%d Hz: TX as set, the base open, PTT goes", (int)ours[i]);
        radio_set_ptt(false);
    }
    knob(7074000);
    gui_run();
    settle();
    check(base.open_tx_writes == writes, "the bands of ours: open TX written %d times (0)",
          base.open_tx_writes - writes);
    none_yet("the end");

    fails += fake_errors;
    printf("%d checks, %d failed\n", checks, fails);
    if (!fails) remove("radio_wr12_test.log");
    return fails != 0;
}
