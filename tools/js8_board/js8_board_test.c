/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Host test of the JS8 board's red card and its "1 new" (src/js8/js8_logic.h): a message to us lights its sender's
 *  card for JS8_RED_MS, a minute, or until the MFK selects the card (seen); a new message makes it red again. A
 *  heartbeat's answer (the SNR report) is neither new nor red, nor is a heartbeat, our own sending, a message to
 *  someone else or one half received. From OlivLoze's fork (F4OLI, 14b215ad, 0d1875b4, 55e3bc5c).
 */

#include "js8/js8_logic.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

static js8_rx_msg_t msg(const char *from, const char *text) {
    js8_rx_msg_t m;

    memset(&m, 0, sizeof(m));
    snprintf(m.from, sizeof(m.from), "%s", from);
    snprintf(m.text, sizeof(m.text), "%s", text);
    m.to_me = true;
    return m;
}

int main(void) {
    const int64_t    t0 = 1763550000000LL;     /* 19 Nov 2026, wall clock ms */
    static js8_red_t reds[JS8_RED_N];
    js8_rx_msg_t     m;

    check(JS8_RED_MS == 60000, "a minute");

    /* what counts as a message to us */
    m = msg("F4OLI", "ON6URE HELLO");
    check(js8_msg_is_new(&m), "a message to us: new");
    m            = msg("SP5CKA", "ON6URE SNR -08");
    m.snr_report = true;
    check(!js8_msg_is_new(&m), "a heartbeat's answer (SNR report): not new, not red");
    m           = msg("SP5GSM", "@HB HEARTBEAT KO02");
    m.heartbeat = true;
    check(!js8_msg_is_new(&m), "a heartbeat: not new");
    m         = msg("F4OLI", "ON6URE HEL");
    m.partial = true;
    check(!js8_msg_is_new(&m), "half received: not yet");
    m    = msg("ON6URE", "F4OLI HELLO");
    m.tx = true;
    check(!js8_msg_is_new(&m), "our own: not new");
    m       = msg("F4OLI", "SN9GK HELLO");
    m.to_me = false;
    check(!js8_msg_is_new(&m), "to someone else: not new");
    m = msg("", "ON6URE HELLO");
    check(!js8_msg_is_new(&m), "no sender: no card");

    /* the red: a minute */
    check(!js8_red_on(reds, "F4OLI", t0), "no message: not red");
    js8_red_mark(reds, "F4OLI", t0);
    check(js8_red_on(reds, "F4OLI", t0), "red as the message comes");
    check(js8_red_on(reds, "f4oli", t0 + 1000), "whatever the case of the call");
    check(js8_red_on(reds, "F4OLI", t0 + 59999), "red 59.999 s after");
    check(!js8_red_on(reds, "F4OLI", t0 + 60000), "plain at 1:00");
    check(!js8_red_on(reds, "F4OLI", t0 + 180000), "plain at 3:00");
    check(!js8_red_on(reds, "SP5CKA", t0 + 1000), "another station: not red");
    check(!js8_red_on(reds, "F4OLI", t0 - 5000), "the clock set back: not red for longer");

    /* the next message: red again, a minute from that one */
    js8_red_mark(reds, "F4OLI", t0 + 3600000);
    check(js8_red_on(reds, "F4OLI", t0 + 3600000 + 59999) && !js8_red_on(reds, "F4OLI", t0 + 3600000 + 60000),
          "red again for a minute after their next message");

    /* seen: the MFK on its card ends it, the next message lights it again */
    js8_red_mark(reds, "SN9GK", t0 + 3600000);
    js8_red_seen(reds, "SN9GK");
    check(!js8_red_on(reds, "SN9GK", t0 + 3600000 + 1000), "selected: seen, plain at once");
    check(js8_red_on(reds, "F4OLI", t0 + 3600000 + 1000), "the other card stays red");
    js8_red_mark(reds, "SN9GK", t0 + 3600000 + 20000);
    check(js8_red_on(reds, "SN9GK", t0 + 3600000 + 21000), "a message after it was seen: red again");
    js8_red_seen(reds, "NOBODY");     /* no such card: nothing happens */

    /* more stations than places: the oldest goes, the newest are red */
    memset(reds, 0, sizeof(reds));
    for (int i = 0; i < JS8_RED_N + 3; i++) {
        char call[JS8_RX_CALL_LEN];

        snprintf(call, sizeof(call), "DL%dXX", i);
        js8_red_mark(reds, call, t0 + i * 100);
    }
    check(!js8_red_on(reds, "DL0XX", t0 + 5000) && !js8_red_on(reds, "DL2XX", t0 + 5000), "full: the oldest went");
    check(js8_red_on(reds, "DL3XX", t0 + 5000) && js8_red_on(reds, "DL34XX", t0 + 5000), "full: the others red");

    printf("js8 board: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
