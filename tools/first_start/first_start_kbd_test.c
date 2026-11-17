/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the first-start guide's Keyboard step (src/first_start_kbd.c; #291): what it shows from what
 *  Bluetooth (bt_dev.h) and the keyboard say, as first_start.c reads them twice a second. The cases:
 *  - no Bluetooth: nothing heard yet, the service not running, the chip off with WiFi, no adapter, the adapter off,
 *    and the adapter still coming after Turn on; a USB keyboard works in each;
 *  - Scan: scanning (with what it found so far), the keyboards to choose (not headphones, not the connected one, at
 *    most FSK_SHOWN), none found;
 *  - a keyboard chosen: what bt_dev's worker says it does, the code to type on it (a passkey, a PIN with letters),
 *    each way a pairing fails (only for a keyboard chosen on the step), connected, and its keys coming;
 *  - what goes first: the code over everything, a scan over a USB keyboard that works;
 *  - the keyboards a scan found kept until the next one, and the entry of one paired at a new address dropped;
 *  - the line typed: ASCII, a layout's letters as UTF-8, the radio's own keys left out, Backspace, a full line;
 *  - the PC build's file for the screenshots.
 */

#include "first_start_kbd.h"

#include <stdio.h>
#include <string.h>

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

static const bt_adapter_t ON = {.seen = true, .bluez = true, .present = true, .powered = true};

static bt_dev_t dev(const char *addr, const char *name, bool input, bool paired, bool connected) {
    bt_dev_t d = {.input = input, .paired = paired, .connected = connected, .audio = !input};

    snprintf(d.addr, sizeof(d.addr), "%s", addr);
    snprintf(d.name, sizeof(d.name), "%s", name);
    return d;
}

static fsk_in_t on(void) {
    fsk_in_t in = {.adapter = ON, .chip_on = true};

    return in;
}

static fsk_state_t state(const fsk_in_t *in) {
    fsk_out_t out;

    fsk_decide(in, &out);
    return out.state;
}

static void no_bluetooth(void) {
    fsk_in_t in = {0};

    in.chip_on = true;
    check(state(&in) == FSK_STARTING, "nothing heard from bt_dev yet: starting");
    in.kbd_ready = true;
    check(state(&in) == FSK_WORKS, "nothing heard yet, a USB keyboard in: it works");

    in = (fsk_in_t){.adapter = {.seen = true}, .chip_on = true};
    check(state(&in) == FSK_NO_SERVICE, "bluetoothd does not answer: no service");
    in.kbd_ready = true;
    check(state(&in) == FSK_WORKS, "no service, a USB keyboard in: it works");

    in = (fsk_in_t){.adapter = {.seen = true, .bluez = true}, .chip_on = false};
    check(state(&in) == FSK_CHIP_OFF, "no adapter with WiFi off: the chip is off");
    in.chip_on = true;
    check(state(&in) == FSK_NO_ADAPTER, "no adapter with WiFi on: none");
    in.turning_on = true;
    check(state(&in) == FSK_STARTING, "no adapter a moment after Turn on: starting");

    in = (fsk_in_t){.adapter = {.seen = true, .bluez = true, .present = true}, .chip_on = true};
    check(state(&in) == FSK_BT_OFF, "the adapter not powered: off");
    in.turning_on = true;
    check(state(&in) == FSK_STARTING, "not powered a moment after Turn on: starting");
    in.turning_on = false;
    in.scanning   = true;
    check(state(&in) == FSK_BT_OFF, "the adapter off: no scan shown, whatever bt_dev says");
    in.kbd_ready = true;
    check(state(&in) == FSK_WORKS, "Bluetooth off, a USB keyboard in: it works");

    in = on();
    in.turning_on = true;
    check(state(&in) == FSK_READY, "on after Turn on: ready at once, not starting");
}

static void scan(void) {
    fsk_in_t  in = on();
    fsk_out_t out;

    check(state(&in) == FSK_READY, "on, nothing asked: ready");
    in.scan_asked = in.scanning = true;
    fsk_decide(&in, &out);
    check(out.state == FSK_SCANNING && out.n_shown == 0, "Scan pressed: scanning, nothing found yet");

    in.devs[in.n++] = dev("AA:00:00:00:00:01", "Headphones", false, false, false);
    in.devs[in.n++] = dev("AA:00:00:00:00:02", "Desk keyboard", true, false, false);
    fsk_decide(&in, &out);
    check(out.state == FSK_SCANNING && out.n_shown == 1 && out.shown[0] == 1,
          "scanning: the keyboard found so far is shown, the headphones not");
    in.scanning = false;
    fsk_decide(&in, &out);
    check(out.state == FSK_LIST && out.n_shown == 1, "the scan over: the keyboard to choose");

    in.devs[in.n++] = dev("AA:00:00:00:00:03", "Old keyboard", true, true, false);
    fsk_decide(&in, &out);
    check(out.n_shown == 2 && out.shown[1] == 2, "a keyboard paired before and not connected is listed too");

    for (int i = 0; i < FSK_SHOWN + 2; i++) {
        char addr[18];

        snprintf(addr, sizeof(addr), "BB:00:00:00:00:%02X", i);
        in.devs[in.n++] = dev(addr, "Keyboard", true, false, false);
    }
    fsk_decide(&in, &out);
    check(out.n_shown == FSK_SHOWN, "more keyboards than the step has room for: FSK_SHOWN");

    in   = on();
    in.scan_asked = true;
    in.devs[in.n++] = dev("AA:00:00:00:00:01", "Headphones", false, false, false);
    check(state(&in) == FSK_NONE_FOUND, "the scan over, only headphones: no keyboard found");
    in.scan_asked = false;
    check(state(&in) == FSK_READY, "headphones listed, no scan asked here: ready");
}

static void pairing(void) {
    fsk_in_t  in = on();
    fsk_out_t out;

    in.devs[in.n++] = dev("AA:00:00:00:00:02", "Desk keyboard", true, false, false);
    in.pair_asked = in.busy = true;
    fsk_decide(&in, &out);
    check(out.state == FSK_PAIRING && out.pairing == FSK_PAIR_BUSY, "chosen, nothing said yet: pairing");
    snprintf(in.activity, sizeof(in.activity), "press the keyboard's pairing key");
    fsk_decide(&in, &out);
    check(out.pairing == FSK_PAIR_KEY, "bt_dev waits for the pairing key: said");
    snprintf(in.activity, sizeof(in.activity), "searching...");
    fsk_decide(&in, &out);
    check(out.pairing == FSK_PAIR_SEARCH, "bt_dev looks for it again: said");
    snprintf(in.activity, sizeof(in.activity), "pairing...");
    fsk_decide(&in, &out);
    check(out.pairing == FSK_PAIR_PAIRING, "pairing: said");
    snprintf(in.activity, sizeof(in.activity), "connecting...");
    fsk_decide(&in, &out);
    check(out.pairing == FSK_PAIR_CONNECTING, "connecting: said");

    snprintf(in.activity, sizeof(in.activity), "type 012345 and Enter on the keyboard");
    fsk_decide(&in, &out);
    check(out.state == FSK_PASSKEY && !strcmp(out.code, "012345"), "a passkey: the code, its leading 0 kept");
    snprintf(in.activity, sizeof(in.activity), "type 4a2B and Enter on the keyboard");
    fsk_decide(&in, &out);
    check(out.state == FSK_PASSKEY && !strcmp(out.code, "4a2B"), "a PIN with letters: as it is");
    in.kbd_ready = true;
    check(state(&in) == FSK_PASSKEY, "the code goes before a USB keyboard that works");
    in.kbd_ready = false;
    snprintf(in.activity, sizeof(in.activity), "type  and Enter");
    check(state(&in) == FSK_PAIRING, "\"type \" with no code: no passkey");

    /* How it ends */
    in.busy = false;
    snprintf(in.activity, sizeof(in.activity), "keyboard not found");
    fsk_decide(&in, &out);
    check(out.state == FSK_FAILED && out.fail == FSK_FAIL_NOT_FOUND, "the keyboard not seen again: failed, not found");
    snprintf(in.activity, sizeof(in.activity), "not found - is it in pairing mode?");
    fsk_decide(&in, &out);
    check(out.fail == FSK_FAIL_NOT_FOUND, "a classic one not found: failed, not found");
    snprintf(in.activity, sizeof(in.activity), "pairing failed");
    fsk_decide(&in, &out);
    check(out.state == FSK_FAILED && out.fail == FSK_FAIL_PAIR, "pairing failed: failed, pair");
    snprintf(in.activity, sizeof(in.activity), "paired, connect failed");
    fsk_decide(&in, &out);
    check(out.state == FSK_FAILED && out.fail == FSK_FAIL_CONNECT, "paired, not connected: failed, connect");
    check(out.n_shown == 1, "failed: the keyboards still listed, to choose again");
    in.pair_asked = false;
    check(state(&in) == FSK_LIST, "a failure of a pairing not asked here: the list");
    in.pair_asked = true;
    snprintf(in.activity, sizeof(in.activity), "%s", "");
    check(state(&in) == FSK_LIST, "chosen, bt_dev done and quiet, nothing connected yet: the list");

    /* It works */
    in.devs[0].paired = in.devs[0].connected = true;
    fsk_decide(&in, &out);
    check(out.state == FSK_CONNECTED && !strcmp(out.name, "Desk keyboard") && out.n_shown == 0,
          "connected, its keys not here yet: connected, by its name, no longer listed");
    in.kbd_ready = true;
    fsk_decide(&in, &out);
    check(out.state == FSK_WORKS && !strcmp(out.name, "Desk keyboard"), "its keys come: works, by its name");
    snprintf(in.activity, sizeof(in.activity), "paired, connect failed");
    check(state(&in) == FSK_WORKS, "it works even when bt_dev said the connect failed (it came back by itself)");

    in = on();
    in.kbd_ready = true;
    fsk_decide(&in, &out);
    check(out.state == FSK_WORKS && !out.name[0], "a USB keyboard: works, no name");
    in.scan_asked = in.scanning = true;
    check(state(&in) == FSK_SCANNING, "Scan pressed with a USB keyboard in: scanning shows");
}

static void merge(void) {
    fsk_kept_t kept = {0};
    bt_dev_t   now[4], out[FSK_DEVS];
    int        n;

    now[0] = dev("AA:00:00:00:00:01", "Headphones", false, true, false);
    now[1] = dev("AA:00:00:00:00:02", "Desk keyboard", true, false, false);
    now[2] = dev("AA:00:00:00:00:03", "Travel keyboard", true, false, false);
    n      = fsk_merge(&kept, now, 3, out);
    check(n == 3 && kept.n == 2, "the scan's keyboards kept, not the paired headphones");

    n = fsk_merge(&kept, now, 1, out);
    check(n == 3 && !strcmp(out[1].addr, "AA:00:00:00:00:02") && !strcmp(out[2].addr, "AA:00:00:00:00:03"),
          "bluez forgot them: still listed, after what bluez has");
    n = fsk_merge(&kept, now, 2, out);
    check(n == 3, "one back in bluez's list: listed once");

    /* Paired at its next address: the entry the scan found goes */
    now[1] = dev("AA:00:00:00:00:12", "Desk keyboard", true, true, true);
    n      = fsk_merge(&kept, now, 2, out);
    check(n == 3 && !strcmp(out[2].addr, "AA:00:00:00:00:03"), "a keyboard paired under its name: its old entry goes");
    check(kept.n == 1, "kept: the one still not paired");

    kept.n = 0;     /* Scan again */
    n      = fsk_merge(&kept, now, 1, out);
    check(n == 1, "a new scan: what the last one found goes");
}

static void echo(void) {
    char line[8] = "";

    check(fsk_echo_add(line, sizeof(line), 'a') && !strcmp(line, "a"), "a letter");
    check(fsk_echo_add(line, sizeof(line), ' ') && !strcmp(line, "a "), "a space");
    check(fsk_echo_add(line, sizeof(line), 0xA9C3) && !strcmp(line, "a \xC3\xA9"), "é as xkb gives it: its bytes");
    check(fsk_echo_back(line) && !strcmp(line, "a "), "Backspace: é goes whole");
    check(!fsk_echo_add(line, sizeof(line), 10) && !fsk_echo_add(line, sizeof(line), 8), "Enter, Backspace: none");
    check(!fsk_echo_add(line, sizeof(line), 27) && !fsk_echo_add(line, sizeof(line), 127), "ESC, Delete: none");
    check(!fsk_echo_add(line, sizeof(line), 0x290) && !fsk_echo_add(line, sizeof(line), 0x296), "VOL, MFK keys: none");
    check(!fsk_echo_add(line, sizeof(line), 0xFF14) && !fsk_echo_add(line, sizeof(line), 0xFD1D),
          "Scroll Lock, Print Screen keysyms: none");
    check(!fsk_echo_add(line, sizeof(line), 0xC3) && !fsk_echo_add(line, sizeof(line), 0x82E2),
          "a lead byte whose letter is cut: none");
    check(!strcmp(line, "a "), "the line as it was");

    snprintf(line, sizeof(line), "%s", "abcdef");
    check(fsk_echo_add(line, sizeof(line), 'g') && !strcmp(line, "abcdefg"), "up to the size");
    check(fsk_echo_add(line, sizeof(line), 'h') && !strcmp(line, "bcdefgh"), "full: the oldest goes");
    check(fsk_echo_add(line, sizeof(line), 0xAC82E2) && !strcmp(line, "efgh\xE2\x82\xAC"), "€: three bytes make room");
    check(fsk_echo_add(line, sizeof(line), 'i') && !strcmp(line, "fgh\xE2\x82\xACi"), "one more: one letter goes");
    snprintf(line, sizeof(line), "%s", "\xC3\xA9xyzuv");
    check(fsk_echo_add(line, sizeof(line), 'w') && !strcmp(line, "xyzuvw"), "é at the front goes whole");
    check(fsk_echo_back(line) && fsk_echo_back(line) && !strcmp(line, "xyzu"), "Backspace twice");
    line[0] = 0;
    check(!fsk_echo_back(line), "Backspace on nothing: nothing");
}

static void fake(const char *path) {
    fsk_in_t in;
    char     typed[16] = "";
    FILE    *f         = fopen(path, "w");

    fputs("# the passkey shot\n"
          "adapter=on\nbusy=1\npair=1\nactivity=type 123456 and Enter on the keyboard\ntyped=hello\n"
          "dev=AA:00:00:00:00:02|Desk keyboard|input\n"
          "dev=AA:00:00:00:00:03|Old keyboard|input,paired,connected\n"
          "dev=bad line\n", f);
    fclose(f);
    check(fsk_fake_load(path, &in, typed, sizeof(typed)), "the file read");
    check(in.adapter.powered && in.chip_on && in.busy && in.pair_asked && !in.scanning, "its flags");
    check(!strcmp(in.activity, "type 123456 and Enter on the keyboard") && !strcmp(typed, "hello"), "its texts");
    check(in.n == 2 && in.devs[0].input && !in.devs[0].paired && in.devs[1].connected && in.devs[1].paired,
          "its devices, a line without a name left out");
    check(!strcmp(in.devs[1].name, "Old keyboard") && !strcmp(in.devs[1].addr, "AA:00:00:00:00:03"), "a name, address");

    f = fopen(path, "w");
    fputs("adapter=chip-off\n", f);
    fclose(f);
    fsk_fake_load(path, &in, NULL, 0);
    check(state(&in) == FSK_CHIP_OFF, "adapter=chip-off");
    f = fopen(path, "w");
    fputs("adapter=no-service\n", f);
    fclose(f);
    fsk_fake_load(path, &in, NULL, 0);
    check(state(&in) == FSK_NO_SERVICE, "adapter=no-service");
    remove(path);
    check(!fsk_fake_load(path, &in, NULL, 0), "no file: false");
}

int main(int argc, char **argv) {
    no_bluetooth();
    scan();
    pairing();
    merge();
    echo();
    fake(argc > 1 ? argv[1] : "first_start_bt.conf");
    printf("first_start_kbd: %i cases, %i wrong\n", cases, bad);
    return bad != 0;
}
