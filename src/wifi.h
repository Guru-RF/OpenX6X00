/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#pragma once

#include <NetworkManager.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus

extern "C" {

#endif

typedef enum {
    WIFI_OFF,
    WIFI_STARTING,
    WIFI_DISCONNECTED,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
} wifi_status_t;

typedef gboolean (*password_validator_fn)(const char *);

typedef struct {
    char                  ssid[33];
    char                  bssid[18];
    uint8_t               strength;
    NM80211Mode           mode;
    bool                  is_connected;
    bool                  known;
    bool                  wpa3_only;        /* SAE only, no WPA2: the chip can't join it */
    password_validator_fn password_validator;
} wifi_ap_info_t;

typedef struct {
    wifi_ap_info_t *ap_arr;
    uint8_t         count;
    bool            is_connected;
} wifi_ap_arr_t;

void wifi_power_setup();

/**
 * Turn WiFi/BT chip on
 */
void wifi_power_on();

/**
 * Turn WiFi/BT chip off
 */
void wifi_power_off();

wifi_ap_arr_t wifi_get_available_access_points();

void wifi_aps_info_delete(wifi_ap_arr_t aps_info);

wifi_status_t wifi_get_status();

void wifi_start_scan();

bool wifi_scanning();

void wifi_add_connection(const char *ssid, const char *password);

void wifi_update_connection(const char *id, const char *password);

void wifi_delete_connection(const char *id);

void wifi_connect(const char *id);

void wifi_disconnect();

bool wifi_get_ipaddr(char **ip_addr, char **gateway);

/* Ethernet: the first wired adapter NetworkManager sees (a USB one), one saved profile */

enum { ETH_IP, ETH_MASK, ETH_GW, ETH_DNS, ETH_FIELDS };

typedef struct {
    bool dhcp;
    char field[ETH_FIELDS][16];     /* dotted quads; gateway and DNS may be empty */
} eth_cfg_t;

typedef struct {
    bool        present;            /* an adapter is plugged in */
    bool        connected;
    const char *state;              /* "connected", "no cable", ... */
    char        iface[16];
    char        addr[20];           /* in use: 192.168.1.20/24 */
    char        gw[16];
    eth_cfg_t   cfg;                /* saved (DHCP when there is no profile) */
} eth_info_t;

/* Any thread: the state as of the last second */
void eth_get_info(eth_info_t *info);

/* True while a wired adapter is connected: WiFi's radio is held off for it (the chip stays on
 * for Bluetooth) and wifi_get_status() says WIFI_OFF. Back on when the cable or the adapter goes. */
bool wifi_eth_active(void);

/* Any thread: the radio has a network, by WiFi or by the wired adapter */
bool net_connected(void);

/* Any thread: NULL or what is wrong. A field alone, or the whole set (netmask normalized to dotted) */
const char *eth_check_field(int field, char *val);
const char *eth_check(eth_cfg_t *cfg);

/* LVGL thread, a checked set: saves the profile and applies it to the adapter */
void eth_apply(const eth_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
