/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - which Bluetooth adapter the radio uses, among those the kernel lists (/sys/class/bluetooth): the
 *  radio's own, its WiFi/BT chip on the MUSB host ("musb-hdrc" in the adapter's sysfs path), or a USB dongle (any
 *  other). With Settings > Bluetooth > USB dongle on, a dongle when one is plugged in, else the radio's own; with it
 *  off hci0, as always. The scripts ask /usr/sbin/bt-dongle, which goes by the same rule. Header only, for bt_dev.c
 *  and its host test (tools/bt_dongle).
 *
 *  From OlivLoze's fork (F4OLI, 07a4140f and 3497515f), with the switch.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[16];      /* hci0, hci1... */
    bool own;           /* the radio's own chip */
} bt_hci_t;

/* An adapter's own entry: hci0, hci1..., not one of its connections (hci0:12) */
static inline bool bt_hci_name_ok(const char *name) {
    return strncmp(name, "hci", 3) == 0 && name[3] >= '0' && name[3] <= '9' && !strchr(name, ':') &&
           strlen(name) < sizeof(((bt_hci_t *)0)->name);
}

/* The adapter's sysfs link, "../../devices/platform/soc/1c19000.usb/musb-hdrc.1.auto/usb1/1-1/1-1:1.0/bluetooth/hci0"
 * on the X6100: the radio's own chip's */
static inline bool bt_hci_own(const char *link) {
    return strstr(link, "musb-hdrc") != NULL;
}

/* The one in use among the n listed, its index; -1 none. Among dongles the lowest number */
static inline int bt_hci_pick(const bt_hci_t *h, int n, bool dongle_on) {
    int own = -1, dongle = -1;

    for (int i = 0; i < n; i++) {
        if (!dongle_on) {
            if (strcmp(h[i].name, "hci0") == 0) {
                return i;
            }
        } else if (!h[i].own) {
            if (dongle < 0 || atoi(h[i].name + 3) < atoi(h[dongle].name + 3)) {
                dongle = i;
            }
        } else if (own < 0) {
            own = i;
        }
    }
    return dongle >= 0 ? dongle : own;
}
