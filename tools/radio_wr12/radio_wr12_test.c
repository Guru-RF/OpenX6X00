/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of radio.c's side of the X6200's WR12 (#134): the real src/radio.c, the settings, the base
 *  and the WR12's driver stood in (fakes.c). What reaches the base is looked at after every write, and at every
 *  wr12_want: with the VFO in use on the WR12's IF, or the WR12 out of its bypass or asked out of it, the power must
 *  be at 0 W, VOX off and KEY Train on already, and nothing keyed (fakes.h). In
 *  order: WR12 on HOST off at the start, on below 54 MHz, the WR12 found; the knob to 98 MHz (held before the base
 *  goes to the IF, I and Q swapped with it, PTT and the modem refused), a band key to 98.1 MHz, the knob back to 7 MHz
 *  (the base on the dial, IQ back, TX held until the WR12's OK to AT+BYPASS=1, PWR, VOX and Train back as they were
 *  after it, PTT then free); the VFOs swapped from A on 98 MHz to B on 7 MHz and back (held before the base's VFO in
 *  use changes); WR12 on HOST off while the WR12 receives; the WR12 unplugged while it receives (145 MHz); Birdie
 *  Cal off the dial and back to it (radio_set_freq, radio_set_freq_dial); 54.5 MHz with TX outside the bands on,
 *  which the hardware could transmit on; a transverter over 2 m, which wins over the WR12.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

static bool held(void) {
    return base.pwr == 0.0f && !base.vox && base.train;
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

/* "start-out": a GUI started while the WR12 may be out of its bypass (after a crash or an update), WR12 on HOST on and
 * the dial on 145.5 MHz. Held from the start, the base's own TX outside its band table shut before the set-up is
 * done, and still while the WR12 is on its way back into its bypass; open with TX as set once it is */
static int start_out(void) {
    if (!freopen("radio_wr12_start.log", "w", stderr)) return 1;
    setvbuf(stderr, NULL, _IONBF, 0);
    fake_cfg_init();
    set_i(cfg_wr12, 1);
    set_i(cfg_band_vfoa_freq, 145500000);
    fake_start_out();
    radio_init();
    radio_start();
    gui_run();

    /* radio_start sends the VFOs before PWR, VOX and Train (the hold is worked out first): until then the base has
     * the library's own start values, the stand-in's -1 W. Looked at from the end of the start on */
    base.violations = 0;
    check(held() && base.open_tx == 0, "start, maybe out: held, open TX %d", base.open_tx);
    none_yet("start, maybe out");
    clock_add(6000);
    said_clear();
    check(!radio_set_ptt(true) && !base.ptt, "start, maybe out: PTT refused");
    wr12_found();
    gui_run();
    base_is("start, set up", A, WR12_IF_HZ, true, 145500000);
    check(held() && base.open_tx == 0, "start, set up: held, open TX %d", base.open_tx);
    wr12_went_out();
    knob(7074000);
    gui_run();
    base_is("start, then 7 MHz", A, 7074000, false, 0);
    check(held() && base.open_tx == 0, "start, then 7 MHz: held until the WR12's OK, open TX %d", base.open_tx);
    wr12_back_in();
    gui_run();
    check(as_set(), "start, the WR12 in its bypass: TX as set, open TX %d", base.open_tx);
    none_yet("start, maybe out");

    fails += fake_errors;
    printf("start-out: %d checks, %d failed\n", checks, fails);
    if (!fails) remove("radio_wr12_start.log");
    return fails != 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "start-out")) return start_out();
    if (!freopen("radio_wr12_test.log", "w", stderr)) return 1;
    setvbuf(stderr, NULL, _IONBF, 0);
    fake_cfg_init();
    radio_init();
    radio_start();
    gui_run();

    /* At the start, WR12 on HOST off, VFO A on 7.074 MHz: the base on the dial, as set */
    base_is("the start", A, 7074000, false, 0);
    check(base.vfo[B] == 14074000 && as_set() && !want.on,
          "the start: VFO B on its own, PWR, VOX and Train as set, the WR12 left alone");

    /* WR12 on HOST on below 54 MHz, the WR12 found: looked for, nothing else moves */
    set_i(cfg_wr12, 1);
    gui_run();
    check(want.on && !want.rx, "on: the WR12 looked for, in its bypass");
    wr12_found();
    gui_run();
    base_is("found, 7 MHz", A, 7074000, false, 0);
    check(as_set(), "found, 7 MHz: TX as set");

    /* The knob to 98 MHz: held before the base goes to the IF, IQ swapped, the WR12 asked out; nothing keys */
    knob(98000000);
    gui_run();
    base_is("98 MHz", A, WR12_IF_HZ, true, 98000000);
    check(held(), "98 MHz: held at 0 W, VOX off, Train on");
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
    base_is("98.1 MHz", A, WR12_IF_HZ, true, 98100000);
    check(held(), "98.1 MHz: held");

    /* The knob back to 7.074 MHz: the base on the dial, IQ back, the WR12 asked into its bypass; held until it
     * answers */
    knob(7074000);
    gui_run();
    base_is("back to 7 MHz", A, 7074000, false, 0);
    check(held(), "back to 7 MHz: still held, the WR12 not seen in its bypass");
    clock_add(6000);
    said_clear();
    check(!radio_set_ptt(true) && !base.ptt, "back to 7 MHz: PTT refused meanwhile");
    check(said("WR12 not in bypass: no TX"), "back to 7 MHz: the refusal said");
    wr12_back_in();
    gui_run();
    check(as_set(), "back in its bypass: PWR %.1f W, VOX %d, Train %d as set", base.pwr, base.vox, base.train);
    check(radio_set_ptt(true) && base.ptt, "back in its bypass: PTT goes");
    radio_set_ptt(false);
    none_yet("back to 7 MHz");

    /* The VFOs swapped: A on 98 MHz in use, B on 7.074 MHz. To B: the base on B's dial, IQ back, the WR12 into its
     * bypass, held until its OK. Back to A: held before the base's VFO in use changes, then A onto the IF */
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
    check(held(), "to B: held until the WR12 is in its bypass");
    wr12_back_in();
    gui_run();
    check(as_set(), "to B, the WR12 in its bypass: TX as set");
    set_i(cfg_band_current_vfo, A);
    gui_run();
    check(base.pwr_at_cur == 0.0f, "back to A: held before the base's VFO in use changed (PWR %.1f W then)",
          base.pwr_at_cur);
    base_is("back to A", A, WR12_IF_HZ, true, 98000000);
    check(held() && base.vfo[B] == 7074000, "back to A: held, B on its own");
    none_yet("the VFOs swapped");

    /* WR12 on HOST off while the WR12 receives: the base back on the dial (98 MHz, the X6200's own FM broadcast), IQ
     * back, the WR12 into its bypass and let go; held until its OK, and then as receive only */
    wr12_went_out();
    gui_run();
    set_i(cfg_wr12, 0);
    gui_run();
    base_is("off while receiving", A, 98000000, false, 0);
    check(!want.on && held(), "off while receiving: the WR12 let go, held");
    wr12_back_in();
    wr12_lost();    /* in its bypass, the port let go */
    gui_run();
    check(held(), "off, 98 MHz: held, receive only");
    knob(7074000);
    gui_run();
    check(as_set(), "off, 7 MHz: TX as set");
    none_yet("off while receiving");

    /* Unplugged while it receives on 145 MHz: the base back on the dial at once, IQ back; held while it may still be
     * out of its bypass, then as receive only */
    set_i(cfg_wr12, 1);
    wr12_found();
    gui_run();
    knob(145000000);
    gui_run();
    base_is("145 MHz", A, WR12_IF_HZ, true, 145000000);
    wr12_went_out();
    wr12_lost();
    gui_run();
    base_is("unplugged at 145 MHz", A, 145000000, false, 0);
    check(held(), "unplugged at 145 MHz: held");
    wr12_gone();
    gui_run();
    check(held(), "unplugged for good, 145 MHz: held, receive only");
    knob(7074000);
    gui_run();
    check(as_set(), "unplugged, 7 MHz: TX as set");
    none_yet("unplugged");

    /* Birdie Cal off the dial at 98 MHz: the base on the run's frequency, IQ back, the WR12 into its bypass, held for
     * the whole run; back on the dial, the base on the IF again, IQ swapped, the WR12 out again, held */
    wr12_found();
    gui_run();
    knob(98000000);
    gui_run();
    wr12_went_out();
    gui_run();
    radio_set_freq(7100000);
    gui_run();
    base_is("Birdie Cal", A, 7100000, false, 0);
    check(held(), "Birdie Cal: held");
    wr12_back_in();
    gui_run();
    check(held(), "Birdie Cal, the WR12 in its bypass: still held for the run");
    radio_set_freq_dial();
    gui_run();
    base_is("Birdie Cal over", A, WR12_IF_HZ, true, 98000000);
    check(held(), "Birdie Cal over: held");
    wr12_went_out();
    knob(7074000);
    wr12_back_in();
    gui_run();
    check(as_set(), "Birdie Cal over, 7 MHz: TX as set");
    none_yet("Birdie Cal");

    /* 54.5 MHz with TX outside the bands on, where the hardware transmits: the WR12's all the same, held */
    set_i(cfg_tx_outside_bands, 1);
    knob(54500000);
    gui_run();
    base_is("54.5 MHz", A, WR12_IF_HZ, true, 54500000);
    check(held(), "54.5 MHz: held, TX outside the bands on or not");
    clock_add(6000);
    said_clear();
    check(!radio_set_ptt(true) && said("WR12 receiving: no TX"), "54.5 MHz: PTT refused, the WR12's word");
    wr12_went_out();
    knob(7074000);
    wr12_back_in();
    gui_run();
    set_i(cfg_tx_outside_bands, 0);
    gui_run();
    check(as_set(), "54.5 MHz, then 7 MHz: TX as set");
    none_yet("54.5 MHz");

    /* A transverter over 144-146 MHz with a shift: it wins, the base on its IF without the swap, the WR12 in its
     * bypass, TX as set */
    set_i(cfg_transverter_0_from, 144000000);
    set_i(cfg_transverter_0_to, 146000000);
    set_i(cfg_transverter_0_shift, 116000000);
    knob(145000000);
    gui_run();
    base_is("a transverter at 145 MHz", A, 29000000, false, 0);
    check(as_set(), "a transverter at 145 MHz: TX as set");
    knob(7074000);
    gui_run();
    none_yet("the end");

    fails += fake_errors;
    printf("%d checks, %d failed\n", checks, fails);
    if (!fails) remove("radio_wr12_test.log");
    return fails != 0;
}
