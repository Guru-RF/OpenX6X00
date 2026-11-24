/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the Bluetooth adapter the GUI uses (src/bt_pick.h, from OlivLoze's fork): with
 *  Settings > Bluetooth > USB dongle off it is hci0 whatever else is there, as always; with it on a dongle when one
 *  is plugged in (the lowest number of several), else the radio's own chip, told by its sysfs link on the MUSB host.
 *  An adapter's connections (hci0:12) are not adapters. bt_dongle_test.py runs /usr/sbin/bt-dongle by the same rule.
 *  See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "bt_pick.h"

#include <stdio.h>

#define OWN_LINK    "../../devices/platform/soc/1c19000.usb/musb-hdrc.1.auto/usb2/2-1/2-1:1.0/bluetooth/hci0"
#define DONGLE_LINK "../../devices/platform/soc/1c1a000.usb/usb1/1-1/1-1:1.0/bluetooth/hci1"

static int checks, failed;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static const char *pick(const bt_hci_t *h, int n, bool on) {
    int i = bt_hci_pick(h, n, on);

    return i < 0 ? "-" : h[i].name;
}

int main(void) {
    check(bt_hci_own(OWN_LINK), "the chip on the MUSB host is the radio's own");
    check(!bt_hci_own(DONGLE_LINK), "an adapter on the EHCI host is a dongle");
    check(!bt_hci_own(""), "an unreadable link: a dongle");

    check(bt_hci_name_ok("hci0") && bt_hci_name_ok("hci12"), "hci0, hci12: adapters");
    check(!bt_hci_name_ok("hci0:12") && !bt_hci_name_ok("hci") && !bt_hci_name_ok(".") &&
          !bt_hci_name_ok("hcix") && !bt_hci_name_ok("hci0123456789abcd"), "a connection, dots and odd names: not");

    /* None */
    check(bt_hci_pick(NULL, 0, false) < 0 && bt_hci_pick(NULL, 0, true) < 0, "no adapter: none, either way");

    /* The radio's own alone, as nearly every radio */
    bt_hci_t own[] = {{"hci0", true}};
    check(!strcmp(pick(own, 1, false), "hci0"), "own alone, off: hci0");
    check(!strcmp(pick(own, 1, true), "hci0"), "own alone, on: hci0, the radio's own");

    /* A dongle that came first (hci0) and the chip after it (hci1), as at a start with the dongle in */
    bt_hci_t first[] = {{"hci1", true}, {"hci0", false}};
    check(!strcmp(pick(first, 2, false), "hci0"), "dongle hci0 and chip hci1, off: hci0, as always");
    check(!strcmp(pick(first, 2, true), "hci0"), "dongle hci0 and chip hci1, on: the dongle");

    /* The chip as hci0 and a dongle plugged in later, hci1 */
    bt_hci_t later[] = {{"hci0", true}, {"hci1", false}};
    check(!strcmp(pick(later, 2, false), "hci0"), "chip hci0 and dongle hci1, off: hci0, the chip, as always");
    check(!strcmp(pick(later, 2, true), "hci1"), "chip hci0 and dongle hci1, on: the dongle");

    /* The chip let go for the dongle: the dongle alone; off, hci0 only if it is hci0 */
    bt_hci_t alone[] = {{"hci1", false}};
    check(!strcmp(pick(alone, 1, true), "hci1"), "the dongle alone as hci1, on: it");
    check(bt_hci_pick(alone, 1, false) < 0, "the dongle alone as hci1, off: no hci0, none (as hci0 always was)");

    /* The chip back after a reset as hci2, no hci0 */
    bt_hci_t reset[] = {{"hci2", true}};
    check(!strcmp(pick(reset, 1, true), "hci2"), "the chip alone as hci2, on: it");

    /* Two dongles: the lowest number, whatever order the directory lists them in */
    bt_hci_t two[] = {{"hci12", false}, {"hci3", true}, {"hci2", false}};
    check(!strcmp(pick(two, 3, true), "hci2"), "two dongles, on: the lowest number");

    printf("bt_pick: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
