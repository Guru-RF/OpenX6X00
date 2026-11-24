/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Bluetooth devices over BlueZ D-Bus (Settings -> Bluetooth): list, scan,
 *  pair, connect, disconnect, remove, and the pairing agent.
 *  Ported from openx6100 (bt_dev.c by SP2KMO and randal007).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT_DEV_MAX 12

typedef struct {
    char addr[18];
    char name[32];
    bool paired;
    bool connected;
    bool audio;     /* headphones or speaker (A2DP sink) */
    bool input;     /* keyboard or other HID device */
    bool phone;     /* Class major 0x02 or Icon "phone": a CAT client */
} bt_dev_t;

typedef struct {
    bool seen;          /* a refresh has run */
    bool bluez;         /* bluetoothd answers on D-Bus */
    bool present;       /* the adapter is there (the radio's own is gone while WiFi has the chip powered down) */
    bool powered;
    bool discoverable;
    bool pairable;
    char addr[18];
} bt_adapter_t;

/* Worker and agent threads; the agent answers pairing only while the
 * pairing window is open (btcat-ctl discoverable on, or a pairing started
 * here). Call once at startup. */
void bt_dev_init(void);

/* Asynchronous refresh of the snapshot read by bt_dev_count/get. */
void bt_dev_refresh(void);
int  bt_dev_count(void);
bool bt_dev_get(int idx, bt_dev_t *out);

/* Connect; a device not paired yet is paired first (headphones in pairing
 * mode, keyboard after its pairing key: a code to type shows in
 * bt_dev_activity). */
void bt_dev_connect(const char *addr);
void bt_dev_disconnect(const char *addr);
/* Disconnect and remove the pairing. */
void bt_dev_forget(const char *addr);

/* 16 s inquiry, LE and classic in turns. Connect ends it early. */
void bt_dev_scan(void);
bool bt_dev_scanning(void);

/* A connect, disconnect or remove is pending or running. */
bool bt_dev_busy(void);

/* What the worker is doing or the last result, "" when idle. */
void bt_dev_activity(char *out, int size);

/* The adapter as the last refresh saw it. */
void bt_dev_adapter(bt_adapter_t *out);

/* The adapter in use: hci0, or with the USB dongle setting on a dongle when one is plugged in, else the radio's own
 * (bt_pick.h). Its name ("hci1") and whether it is a dongle, either may be NULL; false while there is none. Any
 * thread: sysfs only */
bool bt_adapter_sys(char *out, size_t size, bool *dongle);

/* Settings > Bluetooth > USB dongle (BTCAT_DONGLE in btcat.conf, btcat-ctl dongle on|off): read at bt_dev_init, set
 * here by the switch. Always off where the board has no dongle (BOARD_BT_DONGLE) */
void bt_dev_dongle_set(bool on);
bool bt_dev_dongle_get(void);

/* Seconds left in the pairing window (btcat-ctl's), 0 when closed. */
int bt_dev_visible_s(void);

#ifdef __cplusplus
}
#endif
