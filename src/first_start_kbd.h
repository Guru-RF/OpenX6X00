/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the first-start guide's Keyboard step: what it shows, from what Bluetooth (bt_dev.h) and the keyboard
 *  (key_input.h) say (#291). Only the decisions; first_start.c asks bt_dev, shows the step and does what is pressed,
 *  as Settings > Bluetooth > DEVICES does: Scan, a keyboard chosen paired, trusted and connected by bt_dev's worker,
 *  the code to type on it shown. The step works once the keyboard's keys come: what is typed shows on it.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "bt_dev.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FSK_SHOWN    4                  /* keyboards listed at most: the step has room for these */
#define FSK_DEVS     (BT_DEV_MAX * 2)   /* bt_dev's list and the keyboards kept from the scan */

typedef enum {
    FSK_STARTING,       /* nothing heard from Bluetooth yet, or it is coming up after Turn on */
    FSK_NO_SERVICE,     /* the Bluetooth service does not answer */
    FSK_CHIP_OFF,       /* the WiFi/Bluetooth chip is off: WiFi off in Settings > Wireless */
    FSK_NO_ADAPTER,     /* the chip on, and no adapter */
    FSK_BT_OFF,         /* the adapter there and off: Settings > Bluetooth */
    FSK_READY,          /* nothing asked yet: the keyboard in pairing mode, then Scan */
    FSK_SCANNING,
    FSK_LIST,           /* keyboards found: one to choose */
    FSK_NONE_FOUND,     /* the scan found none */
    FSK_PAIRING,        /* the keyboard chosen is being paired or connected (pairing) */
    FSK_PASSKEY,        /* the code to type on it (code) */
    FSK_FAILED,         /* the pairing did not work (fail) */
    FSK_CONNECTED,      /* a Bluetooth keyboard connected, its keys not here yet */
    FSK_WORKS,          /* a keyboard's keys come: type something */
} fsk_state_t;

typedef enum {
    FSK_FAIL_NONE,
    FSK_FAIL_NOT_FOUND, /* it was not seen again after its pairing key */
    FSK_FAIL_PAIR,      /* the pairing was refused or timed out */
    FSK_FAIL_CONNECT,   /* paired, not connected */
} fsk_fail_t;

typedef enum {
    FSK_PAIR_BUSY,      /* anything else bt_dev's worker does first */
    FSK_PAIR_KEY,       /* waiting for the keyboard to show up after its pairing key */
    FSK_PAIR_SEARCH,    /* looking for it again */
    FSK_PAIR_PAIRING,
    FSK_PAIR_CONNECTING,
} fsk_pairing_t;

/* What the step has to go by, read twice a second */
typedef struct {
    bt_adapter_t adapter;
    bool         chip_on;       /* WiFi on: WiFi and Bluetooth share the chip, off together */
    bool         turning_on;    /* Turn on pressed a moment ago: the adapter may still be coming */
    bool         scanning;      /* bt_dev_scanning() */
    bool         busy;          /* bt_dev_busy(): a pairing or a connect running */
    bool         scan_asked;    /* Scan pressed on this step */
    bool         pair_asked;    /* a keyboard chosen on this step */
    bool         kbd_ready;     /* a keyboard's keys come: keyboard_ready() */
    char         activity[48];  /* bt_dev_activity() */
    bt_dev_t     devs[FSK_DEVS];
    int          n;
} fsk_in_t;

typedef struct {
    fsk_state_t   state;
    fsk_fail_t    fail;
    fsk_pairing_t pairing;
    char          code[17];             /* FSK_PASSKEY: what to type on the keyboard */
    char          name[32];             /* FSK_CONNECTED, FSK_WORKS: the Bluetooth keyboard's name; "" a USB one */
    int           shown[FSK_SHOWN];     /* the keyboards to choose from (not connected), their places in devs */
    int           n_shown;
} fsk_out_t;

void fsk_decide(const fsk_in_t *in, fsk_out_t *out);

/* bluez forgets a device found by a scan about 30 s after it, and a keyboard waiting to be chosen would go from the
 * step: kept until the next Scan, as Settings > Bluetooth keeps them. The list to show into out (room for FSK_DEVS):
 * bt_dev's now, then the keyboards kept that it no longer has (not one now paired under the same name, at its next
 * address); kept becomes the keyboards in it not paired */
typedef struct {
    bt_dev_t dev[BT_DEV_MAX];
    int      n;
} fsk_kept_t;

int fsk_merge(fsk_kept_t *kept, const bt_dev_t *now, int n_now, bt_dev_t *out);

/* The step's line of what was typed. A key is added when it is a character: ASCII from the space on, or a letter of
 * the keyboard's layout as its UTF-8 bytes (lv_drivers' xkb.c, as CW Keyboard takes them); the oldest characters
 * go when the line is full. True when the line changed */
bool fsk_echo_add(char *line, size_t size, uint32_t key);
/* Backspace: the last character off. True when there was one */
bool fsk_echo_back(char *line);

/* The PC build's screenshots of the step (tools/screenshots/host_shots.py --data): the state read from a file of
 * name=value lines instead of from Bluetooth, no D-Bus in its sandbox. adapter= on, off, none, chip-off,
 * no-service or unseen; scanning, busy, scan, pair, ready, turning= 1 or 0; activity= bt_dev's text;
 * typed= the line typed; dev= address|name|flags, the flags paired, connected, input, audio, comma separated.
 * False when the file is not there */
bool fsk_fake_load(const char *path, fsk_in_t *in, char *typed, size_t typed_size);
