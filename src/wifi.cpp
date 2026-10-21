/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */


#include "wifi.h"

extern "C" {

#include "msg.h"
#include "params/params.h"
#include "pubsub_ids.h"

#include <aether_radio/x6100_control/low/gpio.h>
#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

}

#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <array>

#define WLAN_IFACE "wlan0"
#define EMPTY_SSID_STR "--"

static GMainLoop *loop;
static NMClient  *client = NULL;
static NMDevice  *device = NULL;

static lv_timer_t *loop_timer;
static lv_timer_t *scan_timer = NULL;
static char        retry_id[33];      /* the network being joined by hand, for the retries */
static int         retry_n;

static wifi_status_t status = WIFI_OFF;
static bool          scanning = false;
static uint64_t      last_scan;
static bool          eth_holds_wifi;   /* a wired adapter is connected: WiFi's radio off for it */

static void loop_iterations_cb(lv_timer_t *);
static void update_scan_status_cb(lv_timer_t *t);

static void setup_nm_client();
static void setup_wifi_device();
static void fill_access_point_info(GBytes *active_ssid, NMAccessPoint *ap, wifi_ap_info_t *ap_info);

static void set_status(wifi_status_t val);

static void device_added_sig_cb(NMClient *client, GObject *device, gpointer user_data);
static void device_state_changed_sig_cb(NMDevice *device, guint new_state, guint old_state, guint reason,
                                        gpointer user_data);
static void active_con_state_changed_sig_cb(NMActiveConnection *active_connection, guint state, guint reason,
                                            gpointer user_data);

static void device_disconnection_cb(GObject *client, GAsyncResult *result, gpointer user_data);
static void scan_request_finishing_cb(GObject *device, GAsyncResult *result, gpointer user_data);
static void connection_adding_cb(GObject *client, GAsyncResult *result, gpointer user_data);
static void connection_adding_and_activating_cb(GObject *client, GAsyncResult *result, gpointer user_data);
static void connection_modify_cb(GObject *connection, GAsyncResult *result, gpointer user_data);
static void connection_delete_cb(GObject *connection, GAsyncResult *result, gpointer user_data);
static void connection_activating_cb(GObject *client, GAsyncResult *result, gpointer user_data);
static void eth_refresh_cb(lv_timer_t *t);
static void wifi_radio_enable(bool on);

static void power_on_cb(lv_timer_t *t) {
    wifi_power_on();
}

/* No network in reach for three minutes, WiFi on and not connected: the chip is powered off
 * and on again, two seconds apart. Now and then it stops seeing any network until it is. */
static void no_network_cb(lv_timer_t *t) {
    static int empty;

    if (status != WIFI_DISCONNECTED || !device || eth_holds_wifi) {
        empty = 0;
        return;
    }
    const GPtrArray *aps = nm_device_wifi_get_access_points(NM_DEVICE_WIFI(device));
    if (aps && aps->len > 0) {
        empty = 0;
        return;
    }
    if (++empty < 3) {
        wifi_start_scan();
        return;
    }
    empty = 0;
    LV_LOG_WARN("No WiFi network found for 3 minutes: the chip is restarted");
    wifi_power_off();
    lv_timer_set_repeat_count(lv_timer_create(power_on_cb, 2000, NULL), 1);
}

void wifi_power_setup() {
    set_status(WIFI_DISCONNECTED);
    loop = g_main_loop_new(NULL, FALSE);
    setup_nm_client();
    if (client != NULL) {
        device = nm_client_get_device_by_iface(client, WLAN_IFACE);
        if (device) {
            setup_wifi_device();
        }
    }
    loop_timer = lv_timer_create(loop_iterations_cb, 30, NULL);
    /* NetworkManager keeps its WirelessEnabled across restarts: a radio held off for an
     * adapter that is gone now would stay off. On; eth_refresh_cb holds it again if need be */
    wifi_radio_enable(true);
    lv_timer_create(eth_refresh_cb, 1000, NULL);
    lv_timer_create(no_network_cb, 60000, NULL);

    if (params.wifi_enabled.x)
        wifi_power_on();
    else
        wifi_power_off();
}

void wifi_power_on() {
    LV_LOG_USER("Power on wifi/bt");
    params_bool_set(&params.wifi_enabled, true);
    x6100_gpio_set(x6100_pin_wifi, 0);
    if (!device) {
        set_status(WIFI_STARTING);
    }
    lv_msg_send(MSG_WIFI_STATE_CHANGED, NULL);
}

void wifi_power_off() {
    LV_LOG_USER("Power off wifi/bt");
    set_status(WIFI_OFF);
    if (device) {
        device = NULL;
    }
    params_bool_set(&params.wifi_enabled, false);
    x6100_gpio_set(x6100_pin_wifi, 1);
    if (scan_timer) {
        lv_timer_del(scan_timer);
        scan_timer = NULL;
    }
    scanning = false;
}

wifi_status_t wifi_get_status() {
    return eth_holds_wifi ? WIFI_OFF : status;
}

bool wifi_eth_active(void) {
    return eth_holds_wifi;
}

/* NetworkManager's WirelessEnabled (the rfkill soft switch of wlan0), asynchronously */
static void wifi_radio_enable(bool on) {
    if (!client) {
        return;
    }
    nm_client_dbus_set_property(client, NM_DBUS_PATH, NM_DBUS_INTERFACE, "WirelessEnabled",
                                g_variant_new_boolean(on), -1, NULL, NULL, NULL);
}

void wifi_start_scan() {
    if (device != NULL) {
        last_scan = nm_device_wifi_get_last_scan(NM_DEVICE_WIFI(device));
        nm_device_wifi_request_scan_async(NM_DEVICE_WIFI(device), NULL, scan_request_finishing_cb, loop);
    }
}

bool wifi_scanning() {
    return scanning;
}

wifi_ap_arr_t wifi_get_available_access_points() {
    const GPtrArray *aps;
    NMAccessPoint   *active_ap = NULL;
    GBytes          *active_ssid = NULL;
    uint             i;

    wifi_ap_arr_t aps_info;

    aps_info.count = 0;
    aps_info.ap_arr = NULL;
    aps_info.is_connected = false;

    if (device != NULL) {
        /* Get active AP */
        if (nm_device_get_state(device) == NM_DEVICE_STATE_ACTIVATED) {
            if ((active_ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(device)))) {
                active_ssid = nm_access_point_get_ssid(active_ap);
            }
        }

        aps = nm_device_wifi_get_access_points(NM_DEVICE_WIFI(device));

        // map ssid -> [strength, ap_vec_id]
        std::map<std::string, std::array<uint, 2>> ssid_strength_map{};
        std::vector<wifi_ap_info_t>                ap_info_vec;

        for (i = 0; i < aps->len; i++) {
            wifi_ap_info_t ap_info;
            NMAccessPoint *ap = NM_ACCESS_POINT(g_ptr_array_index(aps, i));
            fill_access_point_info(active_ssid, ap, &ap_info);

            std::string ssid = ap_info.ssid;

            auto search = ssid_strength_map.find(ssid);
            if ((search != ssid_strength_map.end()) && (ssid.compare(EMPTY_SSID_STR) !=0 )) {
                if (search->second[0] < ap_info.strength) {
                    ap_info_vec[ssid_strength_map[ssid][1]] = ap_info;
                    ssid_strength_map[ssid] = {ap_info.strength, i};
                }

            } else {
                ssid_strength_map[ssid] = {
                    {ap_info.strength, ap_info_vec.size()}
                };
                ap_info_vec.push_back(ap_info);
            }
        }

        aps_info.count = ssid_strength_map.size();
        aps_info.ap_arr = (wifi_ap_info_t *)malloc(sizeof(wifi_ap_info_t) * aps_info.count);
        for (i = 0; i < aps_info.count; i++) {
            aps_info.ap_arr[i] = ap_info_vec[i];
            aps_info.is_connected |= aps_info.ap_arr[i].is_connected;
        }
    }
    return aps_info;
}

void wifi_aps_info_delete(wifi_ap_arr_t aps_info) {
    if (aps_info.ap_arr != NULL) {
        free(aps_info.ap_arr);
    }
}

void wifi_add_connection(const char *ssid, const char *password) {
    snprintf(retry_id, sizeof(retry_id), "%s", ssid);
    retry_n = 0;
    NMConnection              *connection;
    NMSettingConnection       *s_con;
    NMSettingWireless         *s_wireless;
    NMSettingWirelessSecurity *s_wsec;
    NMSettingIP4Config        *s_ip4;
    GBytes                    *g_ssid;
    char                      *uuid;

    /* Create a new connection object */
    connection = nm_simple_connection_new();

    /* Build up the 'connection' Setting */
    s_con = (NMSettingConnection *)nm_setting_connection_new();
    uuid = nm_utils_uuid_generate();
    // clang-format off
    g_object_set(G_OBJECT(s_con),
				 NM_SETTING_CONNECTION_UUID, uuid,
				 NM_SETTING_CONNECTION_ID, ssid,
                 NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
				 NM_SETTING_CONNECTION_AUTOCONNECT, TRUE,
                 NM_SETTING_CONNECTION_AUTOCONNECT_RETRIES, 0,   // 0 = forever, see fix_autoconnect_retries()
				 NM_SETTING_CONNECTION_INTERFACE_NAME, WLAN_IFACE,
				 NULL);
    // clang-format on
    g_free(uuid);
    nm_connection_add_setting(connection, NM_SETTING(s_con));

    /* Build up the 'wireless' Setting */
    // nm_utils_ap_mode_security_valid
    g_ssid = g_bytes_new(ssid, strlen(ssid));
    s_wireless = (NMSettingWireless *)nm_setting_wireless_new();
    // clang-format off
    g_object_set(G_OBJECT(s_wireless),
				 NM_SETTING_WIRELESS_SSID, g_ssid,
				 NM_SETTING_WIRELESS_MODE, NM_SETTING_WIRELESS_MODE_INFRA,
				 NULL);
    // clang-format on
    nm_connection_add_setting(connection, NM_SETTING(s_wireless));

    /* Wifi security */
    if (password) {
        s_wsec = (NMSettingWirelessSecurity *)nm_setting_wireless_security_new();
        // clang-format off
        g_object_set(G_OBJECT(s_wsec),
                    NM_SETTING_WIRELESS_SECURITY_AUTH_ALG, "open",
                    NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk",
                    NM_SETTING_WIRELESS_SECURITY_PSK, password,
                    NULL);
        // clang-format on
        nm_connection_add_setting(connection, NM_SETTING(s_wsec));
    }

    /* Build up the 'ipv4' Setting */
    s_ip4 = (NMSettingIP4Config *)nm_setting_ip4_config_new();
    // clang-format off
    g_object_set(G_OBJECT(s_ip4),
			     NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
				 NULL);
    // clang-format on
    nm_connection_add_setting(connection, NM_SETTING(s_ip4));

    /* No IPv6: the kernel has none, and NetworkManager would retry its address every 2 s */
    NMSettingIP6Config *s_ip6 = (NMSettingIP6Config *)nm_setting_ip6_config_new();
    g_object_set(G_OBJECT(s_ip6), NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_DISABLED, NULL);
    nm_connection_add_setting(connection, NM_SETTING(s_ip6));

    LV_LOG_USER("Adding connection");
    nm_client_add_and_activate_connection_async(client, connection, device, NULL, NULL,
                                                connection_adding_and_activating_cb, NULL);
    g_object_unref(connection);
}

void wifi_update_connection(const char *id, const char *password) {
    NMRemoteConnection        *rem_con = NULL;
    NMConnection              *new_connection;
    NMSettingWirelessSecurity *s_wsec;
    gboolean                   temporary = FALSE;

    rem_con = nm_client_get_connection_by_id(client, id);
    if (rem_con) {
        new_connection = nm_simple_connection_new_clone(NM_CONNECTION(rem_con));
        nm_connection_remove_setting(new_connection, NM_TYPE_SETTING_WIRELESS_SECURITY);
        if (password) {
            s_wsec = (NMSettingWirelessSecurity *)nm_setting_wireless_security_new();
            // clang-format off
            g_object_set(s_wsec,
                        NM_SETTING_WIRELESS_SECURITY_AUTH_ALG, "open",
                        NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk",
                        NM_SETTING_WIRELESS_SECURITY_PSK, password,
                        NULL);
            // clang-format on
            nm_connection_add_setting(new_connection, NM_SETTING(s_wsec));
        }
        nm_connection_replace_settings_from_connection(NM_CONNECTION(rem_con), new_connection);
        nm_remote_connection_commit_changes_async(rem_con, !temporary, NULL, connection_modify_cb, NULL);
        g_object_unref(new_connection);
    } else {
        LV_LOG_ERROR("Connection %s not found", id);
    }
}

void wifi_delete_connection(const char *id) {
    NMRemoteConnection *rem_con;

    rem_con = nm_client_get_connection_by_id(client, id);
    nm_remote_connection_delete_async(rem_con, NULL, connection_delete_cb, NULL);
}

void wifi_connect(const char *id) {
    NMRemoteConnection  *rem_con = NULL;

    snprintf(retry_id, sizeof(retry_id), "%s", id);
    retry_n = 0;
    NMSettingConnection *s_con;
    NMConnection        *connection = NULL;
    gboolean             temporary = FALSE;
    const char          *uuid;
    const char          *password;

    if (device != NULL) {
        rem_con = nm_client_get_connection_by_id(client, id);
        if (rem_con != NULL) {
            nm_client_activate_connection_async(client, NM_CONNECTION(rem_con), device, NULL, NULL,
                                                connection_activating_cb, loop);
        } else {
            LV_LOG_WARN("Connection with id=%s is not found", id);
        }
    }
}

void wifi_disconnect() {
    if (device != NULL) {
        nm_device_disconnect_async(device, NULL, device_disconnection_cb, NULL);
    }
}

bool wifi_get_ipaddr(char **ip_addr, char **gateway) {
    NMIPConfig *ip_cfg;
    if (device) {
        ip_cfg = nm_device_get_ip4_config(device);
        if (ip_cfg) {
            const char * gw = nm_ip_config_get_gateway(ip_cfg);
            if(gw) {
                strcpy(*gateway, gw);
                GPtrArray *addresses = nm_ip_config_get_addresses(ip_cfg);
                if (addresses->len > 0) {
                    NMIPAddress *address = (NMIPAddress *)g_ptr_array_index(addresses, 0);
                    strcpy(*ip_addr, nm_ip_address_get_address(address));
                }
                return true;
            }
        }
    }
    return false;
}

static void loop_iterations_cb(lv_timer_t *t) {
    g_main_context_iteration(g_main_loop_get_context(loop), FALSE);
}

static void update_scan_status_cb(lv_timer_t *t) {
    uint64_t val;

    if (device) {
        val = nm_device_wifi_get_last_scan(NM_DEVICE_WIFI(device));
        if (val != last_scan) {
            LV_LOG_USER("Scan is finished");
            scanning = false;
            last_scan = val;
            lv_timer_del(scan_timer);
            scan_timer = NULL;
            lv_msg_send(MSG_WIFI_STATE_CHANGED, NULL);
        }
    }
}

/*
 * Networks saved by older versions have autoconnect-retries=1: one failed
 * attempt (e.g. while a Bluetooth voice link starves the shared RTL8723BU
 * radio) blocks the network, and NM no longer reconnects even after the
 * chip comes back. Retry forever instead.
 */
static void fix_autoconnect_retries() {
    const GPtrArray *cons = nm_client_get_connections(client);

    for (guint i = 0; i < cons->len; i++) {
        NMRemoteConnection  *rem_con = (NMRemoteConnection *)g_ptr_array_index(cons, i);
        NMSettingConnection *s_con = nm_connection_get_setting_connection(NM_CONNECTION(rem_con));

        if (!s_con || nm_setting_connection_get_autoconnect_retries(s_con) != 1 ||
            strcmp(nm_setting_connection_get_connection_type(s_con), NM_SETTING_WIRELESS_SETTING_NAME) != 0)
            continue;
        g_object_set(G_OBJECT(s_con), NM_SETTING_CONNECTION_AUTOCONNECT_RETRIES, 0, NULL);
        nm_remote_connection_commit_changes_async(rem_con, TRUE, NULL, connection_modify_cb, NULL);
    }
}

static void setup_nm_client() {
    GError *error = NULL;

    /* Get NMClient object */
    client = nm_client_new(NULL, &error);
    if (!client) {
        LV_LOG_ERROR("Could not create NMClient: %s.", error->message);
        g_error_free(error);
        return;
    }
    g_signal_connect(client, "device-added", G_CALLBACK(device_added_sig_cb), NULL);
    fix_autoconnect_retries();
}

static void setup_wifi_device() {
    NMActiveConnection *active_con;

    LV_LOG_USER("Setup wlan0 device");
    g_signal_connect(device, "state-changed", G_CALLBACK(device_state_changed_sig_cb), NULL);
    active_con = nm_device_get_active_connection(device);
    if (active_con) {
        g_signal_connect(active_con, "state-changed", G_CALLBACK(active_con_state_changed_sig_cb), NULL);
        set_status(WIFI_CONNECTED);
    } else {
        set_status(WIFI_DISCONNECTED);
    }
}

static void device_added_sig_cb(NMClient *client, GObject *dev, gpointer user_data) {
    NMDevice           *nm_dev;
    NMActiveConnection *active_con;
    nm_dev = NM_DEVICE(dev);
    if (strcmp(nm_device_get_iface(nm_dev), WLAN_IFACE) == 0) {
        device = nm_dev;
        setup_wifi_device();
    }
}

static void fill_access_point_info(GBytes *active_ssid, NMAccessPoint *ap, wifi_ap_info_t *ap_info) {
    NMRemoteConnection *rem_con;
    guint32             flags, wpa_flags, rsn_flags;
    guint8              strength;
    GBytes             *ssid;
    const char         *hwaddr;
    NM80211Mode         mode;
    char               *ssid_str, *wpa_flags_str, *rsn_flags_str;

    /* Get AP properties */
    flags = nm_access_point_get_flags(ap);
    wpa_flags = nm_access_point_get_wpa_flags(ap);
    rsn_flags = nm_access_point_get_rsn_flags(ap);
    ssid = nm_access_point_get_ssid(ap);
    hwaddr = nm_access_point_get_bssid(ap);
    mode = nm_access_point_get_mode(ap);
    strength = nm_access_point_get_strength(ap);

    /* Convert to strings */
    if (ssid)
        ssid_str = nm_utils_ssid_to_utf8((const guint8 *)g_bytes_get_data(ssid, NULL), g_bytes_get_size(ssid));
    else
        ssid_str = g_strdup(EMPTY_SSID_STR);
    strcpy(ap_info->bssid, hwaddr);
    strcpy(ap_info->ssid, ssid_str);
    ap_info->strength = strength;
    ap_info->mode = mode;
    ap_info->is_connected = active_ssid && (g_bytes_compare(ssid, active_ssid) == 0);

    rem_con = nm_client_get_connection_by_id(client, ssid_str);
    ap_info->known = rem_con != NULL;

    /* Set password validator */
    if (rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_PSK) {
        // WPA2
        ap_info->password_validator = nm_utils_wpa_psk_valid;
    } else if (flags == NM_802_11_AP_FLAGS_NONE) {
        // Open
        ap_info->password_validator = NULL;
    }
    /* WPA3 only (SAE without WPA2-PSK): the window says so instead of asking a password */
    ap_info->wpa3_only = (rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_SAE) && !(rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_PSK);

    g_free(ssid_str);
}

static void set_status(wifi_status_t val) {
    wifi_status_t prev_val = status;
    status = val;
    if (val != prev_val) {
        lv_msg_send(MSG_WIFI_STATE_CHANGED, NULL);
    }
}

static void scan_request_finishing_cb(GObject *device, GAsyncResult *result, gpointer user_data) {
    GError *error = NULL;
    bool    res;

    res = nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(device), result, &error);

    if (!res) {
        LV_LOG_ERROR("Error during starting scan: %s", error->message);
        g_error_free(error);
    } else {
        LV_LOG_USER("Scan is started");
        scanning = true;
        scan_timer = lv_timer_create(update_scan_status_cb, 500, NULL);
    }
    lv_msg_send(MSG_WIFI_STATE_CHANGED, NULL);
    g_main_loop_quit(loop);
}

static void connection_adding_cb(GObject *client, GAsyncResult *result, gpointer user_data) {
    NMRemoteConnection *remote;
    GError             *error = NULL;

    /* NM responded to our request; either handle the resulting error or
     * print out the object path of the connection we just added.
     */
    remote = nm_client_add_connection_finish(NM_CLIENT(client), result, &error);

    if (error) {
        LV_LOG_ERROR("Error adding connection: %s (code: %zu, domain: %zu)", error->message, error->code,
                     error->domain);
        // stort password - (code: 7, domain: 53)
        g_error_free(error);
    } else {
        LV_LOG_USER("Added: %s\n", nm_connection_get_path(NM_CONNECTION(remote)));
        g_object_unref(remote);
    }

    /* Tell the mainloop we're done and we can quit now */
    g_main_loop_quit(loop);
}

static void connection_adding_and_activating_cb(GObject *client, GAsyncResult *result, gpointer user_data) {
    NMActiveConnection *active_con;
    GError             *error = NULL;

    active_con = nm_client_add_and_activate_connection_finish(NM_CLIENT(client), result, &error);

    if (error) {
        LV_LOG_ERROR("Error adding and activating connection: %s (code: %zu, domain: %zu)", error->message, error->code,
                     error->domain);
        // stort password - (code: 7, domain: 53)
        g_error_free(error);
    } else {
        LV_LOG_USER("Activated: %s\n", nm_active_connection_get_id(active_con));
        set_status(WIFI_CONNECTING);
        g_signal_connect(active_con, "state-changed", G_CALLBACK(active_con_state_changed_sig_cb), NULL);
        g_object_unref(active_con);
    }

    /* Tell the mainloop we're done and we can quit now */
    g_main_loop_quit(loop);
}

static void connection_modify_cb(GObject *connection, GAsyncResult *result, gpointer user_data) {
    GError *error = NULL;

    if (!nm_remote_connection_commit_changes_finish(NM_REMOTE_CONNECTION(connection), result, &error)) {
        LV_LOG_ERROR(("Error: Failed to modify connection '%s': %s"), nm_connection_get_id(NM_CONNECTION(connection)),
                     error->message);
    } else {
        LV_LOG_USER(("Connection '%s' (%s) successfully modified.\n"), nm_connection_get_id(NM_CONNECTION(connection)),
                    nm_connection_get_uuid(NM_CONNECTION(connection)));
    }
    g_main_loop_quit(loop);
}

static void connection_delete_cb(GObject *connection, GAsyncResult *result, gpointer user_data) {
    GError *error = NULL;

    if (!nm_remote_connection_delete_finish(NM_REMOTE_CONNECTION(connection), result, &error)) {
        LV_LOG_ERROR(("Error: Failed to delete connection '%s': %s"), nm_connection_get_id(NM_CONNECTION(connection)),
                     error->message);
    } else {
        LV_LOG_USER(("Connection '%s' (%s) successfully deleted.\n"), nm_connection_get_id(NM_CONNECTION(connection)),
                    nm_connection_get_uuid(NM_CONNECTION(connection)));
    }
    g_main_loop_quit(loop);
}

static void device_state_changed_sig_cb(NMDevice *device, guint new_state, guint old_state, guint reason,
                                        gpointer user_data) {
    LV_LOG_INFO("Old state: %zu, new state: %zu, reason: %zu", old_state, new_state, reason);
    switch (new_state) {
    case NM_DEVICE_STATE_FAILED:
        if (reason == NM_DEVICE_STATE_REASON_NO_SECRETS) {
            /* The handshake fails now and then with the right password (the chip shared with
             * Bluetooth, a weak signal): four tries before the password is doubted */
            NMRemoteConnection *rc = retry_id[0] ? nm_client_get_connection_by_id(client, retry_id) : NULL;

            if (rc && retry_n < 3) {
                retry_n++;
                msg_update_text_fmt("Connecting... (try %d/4)", retry_n + 1);
                nm_client_activate_connection_async(client, NM_CONNECTION(rc), device, NULL, NULL,
                                                    connection_activating_cb, loop);
            } else {
                retry_n = 0;
                LV_LOG_WARN("Wrong password");
                msg_update_text_fmt("Wrong WiFi password");
            }
        }
        break;
    case NM_DEVICE_STATE_ACTIVATED:
        retry_n = 0;
        set_status(WIFI_CONNECTED);
        break;
    case NM_DEVICE_STATE_DISCONNECTED:
        set_status(WIFI_DISCONNECTED);
        break;
    }
}

static void active_con_state_changed_sig_cb(NMActiveConnection *active_connection, guint state, guint reason,
                                            gpointer user_data) {
    LV_LOG_INFO("Active con state change -  state: %zu, reason: %zu", state, reason);
    lv_msg_send(MSG_WIFI_STATE_CHANGED, NULL);
}

static void connection_activating_cb(GObject *client, GAsyncResult *result, gpointer user_data) {
    NMActiveConnection *active_con;
    GError             *error = NULL;

    active_con = nm_client_activate_connection_finish(NM_CLIENT(client), result, &error);

    if (error) {
        LV_LOG_ERROR("Error activating connection: %s (code: %zu, domain: %zu)", error->message, error->code,
                     error->domain);
        g_error_free(error);
    } else {
        LV_LOG_USER("Activated: %s\n", nm_active_connection_get_id(active_con));
        set_status(WIFI_CONNECTING);
        g_signal_connect(active_con, "state-changed", G_CALLBACK(active_con_state_changed_sig_cb), NULL);
        g_object_unref(active_con);
    }

    g_main_loop_quit(loop);
}

static void device_disconnection_cb(GObject *device, GAsyncResult *result, gpointer user_data) {
    GError *error = NULL;
    bool    res;

    res = nm_device_disconnect_finish(NM_DEVICE(device), result, &error);

    if (!res) {
        LV_LOG_ERROR("Error disconnecting: %s", error->message);
        g_error_free(error);
    } else {
        LV_LOG_USER("%s disconnected", nm_device_get_iface(NM_DEVICE(device)));
        set_status(WIFI_DISCONNECTED);
    }
    g_main_loop_quit(loop);
}

/*
 * Ethernet. One saved profile, "Ethernet", for any wired adapter (no interface name or MAC in it:
 * a USB adapter can be swapped). Routes keep the NetworkManager metrics: wired 100, WiFi 600, so
 * with a gateway on both the cable carries the traffic and WiFi stays up. Without a gateway the
 * cable reaches its own subnet only and the default route stays on WiFi.
 */

#define ETH_CON_ID "Ethernet"

static std::mutex eth_mutex;
static eth_info_t eth_info = {false, false, "no adapter"};

static NMDevice *eth_device() {
    if (!client) {
        return NULL;
    }
    const GPtrArray *devs = nm_client_get_devices(client);

    for (guint i = 0; i < devs->len; i++) {
        NMDevice *d = NM_DEVICE(g_ptr_array_index(devs, i));

        /* a phone's USB tethering is not the adapter: it has its own profile (usb-tether) */
        if (NM_IS_DEVICE_ETHERNET(d) && g_strcmp0(nm_device_get_driver(d), "rndis_host") != 0) {
            return d;
        }
    }
    return NULL;
}

static void mask_from_prefix(guint prefix, char *out) {
    struct in_addr a;

    a.s_addr = htonl(prefix ? 0xffffffffu << (32 - prefix) : 0);
    inet_ntop(AF_INET, &a, out, 16);
}

static void eth_refresh_cb(lv_timer_t *t) {
    eth_info_t in = {};
    NMDevice  *d = eth_device();

    in.cfg.dhcp = true;
    in.state = "no adapter";
    if (d) {
        NMDeviceState st = nm_device_get_state(d);

        in.present = true;
        in.connected = st == NM_DEVICE_STATE_ACTIVATED;
        snprintf(in.iface, sizeof(in.iface), "%s", nm_device_get_iface(d));
        if (in.connected) {
            in.state = "connected";
        } else if (!nm_device_ethernet_get_carrier(NM_DEVICE_ETHERNET(d))) {
            in.state = "no cable";
        } else if (st > NM_DEVICE_STATE_DISCONNECTED && st < NM_DEVICE_STATE_ACTIVATED) {
            in.state = "connecting";
        } else {
            in.state = "not connected";
        }
        NMIPConfig *ip = in.connected ? nm_device_get_ip4_config(d) : NULL;

        if (ip) {
            GPtrArray  *addrs = nm_ip_config_get_addresses(ip);
            const char *gw = nm_ip_config_get_gateway(ip);

            if (addrs->len) {
                NMIPAddress *a = (NMIPAddress *)g_ptr_array_index(addrs, 0);

                snprintf(in.addr, sizeof(in.addr), "%s/%u", nm_ip_address_get_address(a), nm_ip_address_get_prefix(a));
            }
            snprintf(in.gw, sizeof(in.gw), "%s", gw ? gw : "");
        }
    }

    NMRemoteConnection *rc = client ? nm_client_get_connection_by_id(client, ETH_CON_ID) : NULL;
    NMSettingIPConfig  *s  = rc ? nm_connection_get_setting_ip4_config(NM_CONNECTION(rc)) : NULL;

    if (s && strcmp(nm_setting_ip_config_get_method(s), NM_SETTING_IP4_CONFIG_METHOD_MANUAL) == 0) {
        in.cfg.dhcp = false;
        if (nm_setting_ip_config_get_num_addresses(s)) {
            NMIPAddress *a = nm_setting_ip_config_get_address(s, 0);

            snprintf(in.cfg.field[ETH_IP], 16, "%s", nm_ip_address_get_address(a));
            mask_from_prefix(nm_ip_address_get_prefix(a), in.cfg.field[ETH_MASK]);
        }
        if (nm_setting_ip_config_get_gateway(s)) {
            snprintf(in.cfg.field[ETH_GW], 16, "%s", nm_setting_ip_config_get_gateway(s));
        }
        if (nm_setting_ip_config_get_num_dns(s)) {
            snprintf(in.cfg.field[ETH_DNS], 16, "%s", nm_setting_ip_config_get_dns(s, 0));
        }
    }

    bool changed;
    {
        std::lock_guard<std::mutex> lock(eth_mutex);
        changed  = in.present != eth_info.present || in.connected != eth_info.connected;
        eth_info = in;
    }
    /* A wired connection takes over: WiFi's radio is held off while it lasts (a radio the
     * Bluetooth chip shares, and two routes to the same LAN confuse the TCI clients); the chip
     * stays powered for Bluetooth. The cable or the adapter gone: the radio comes back */
    if (in.connected != eth_holds_wifi) {
        eth_holds_wifi = in.connected;
        LV_LOG_USER("Ethernet %s: WiFi radio %s", in.connected ? "connected" : "gone", in.connected ? "off" : "on");
        wifi_radio_enable(!in.connected);
        if (in.connected && scan_timer) {
            lv_timer_del(scan_timer);
            scan_timer = NULL;
            scanning   = false;
        }
    }
    if (changed) {
        lv_msg_send(MSG_WIFI_STATE_CHANGED, NULL);   /* the info bar's symbol */
    }
}

void eth_get_info(eth_info_t *info) {
    std::lock_guard<std::mutex> lock(eth_mutex);
    *info = eth_info;
}

static int mask_prefix(const char *mask) {
    struct in_addr a;

    inet_pton(AF_INET, mask, &a);
    return __builtin_popcount(a.s_addr);
}

const char *eth_check_field(int field, char *val) {
    static const char *bad[ETH_FIELDS] = {"IP address is not valid", "Netmask is not valid (255.255.255.0 or /24)",
                                          "Gateway is not valid", "DNS is not valid"};
    struct in_addr     a;
    char              *p = val;

    while (*p == ' ') p++;
    memmove(val, p, strlen(p) + 1);
    for (p = val + strlen(val); p > val && p[-1] == ' '; p--) p[-1] = 0;
    if (!*val) {
        return NULL;    /* eth_check() says which ones are needed */
    }
    if (field == ETH_MASK) {
        const char *digits = val[0] == '/' ? val + 1 : val;

        if (*digits && strlen(digits) <= 2 && strspn(digits, "0123456789") == strlen(digits)) {
            int prefix = atoi(digits);

            if (prefix < 1 || prefix > 32) {
                return bad[field];
            }
            mask_from_prefix(prefix, val);
            return NULL;
        }
        if (inet_pton(AF_INET, val, &a) != 1) {
            return bad[field];
        }
        uint32_t inv = ~ntohl(a.s_addr);

        return a.s_addr && !(inv & (inv + 1)) ? NULL : bad[field];     /* ones, then zeros */
    }
    if (inet_pton(AF_INET, val, &a) != 1) {
        return bad[field];
    }
    uint8_t first = ntohl(a.s_addr) >> 24;

    if (first == 0 || first == 127 || first >= 224) {
        return bad[field];
    }
    inet_ntop(AF_INET, &a, val, 16);
    return NULL;
}

const char *eth_check(eth_cfg_t *cfg) {
    if (cfg->dhcp) {
        return NULL;
    }
    for (int i = 0; i < ETH_FIELDS; i++) {
        const char *err = eth_check_field(i, cfg->field[i]);

        if (err) {
            return err;
        }
    }
    if (!cfg->field[ETH_IP][0] || !cfg->field[ETH_MASK][0]) {
        return "Static needs an IP address and a netmask";
    }
    struct in_addr ip, mask, gw;

    inet_pton(AF_INET, cfg->field[ETH_IP], &ip);
    inet_pton(AF_INET, cfg->field[ETH_MASK], &mask);
    if (mask_prefix(cfg->field[ETH_MASK]) <= 30) {
        uint32_t host = ip.s_addr & ~mask.s_addr;

        if (host == 0 || host == ~mask.s_addr) {
            return "The IP address is the network or broadcast address of its subnet";
        }
    }
    if (cfg->field[ETH_GW][0]) {
        inet_pton(AF_INET, cfg->field[ETH_GW], &gw);
        if ((gw.s_addr & mask.s_addr) != (ip.s_addr & mask.s_addr) || gw.s_addr == ip.s_addr) {
            return "The gateway is not in the subnet of the IP address";
        }
    }
    return NULL;
}

static NMConnection *eth_connection_new(const eth_cfg_t *cfg, const char *uuid) {
    NMConnection        *con = nm_simple_connection_new();
    NMSettingConnection *s_con = (NMSettingConnection *)nm_setting_connection_new();
    NMSettingIPConfig   *s_ip4 = (NMSettingIPConfig *)nm_setting_ip4_config_new();
    NMSettingIPConfig   *s_ip6 = (NMSettingIPConfig *)nm_setting_ip6_config_new();

    g_object_set(G_OBJECT(s_con), NM_SETTING_CONNECTION_UUID, uuid, NM_SETTING_CONNECTION_ID, ETH_CON_ID,
                 NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRED_SETTING_NAME, NM_SETTING_CONNECTION_AUTOCONNECT, TRUE, NULL);
    nm_connection_add_setting(con, NM_SETTING(s_con));
    nm_connection_add_setting(con, nm_setting_wired_new());

    if (cfg->dhcp) {
        g_object_set(G_OBJECT(s_ip4), NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO, NULL);
    } else {
        NMIPAddress *a = nm_ip_address_new(AF_INET, cfg->field[ETH_IP], mask_prefix(cfg->field[ETH_MASK]), NULL);

        g_object_set(G_OBJECT(s_ip4), NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_MANUAL, NULL);
        nm_setting_ip_config_add_address(s_ip4, a);
        nm_ip_address_unref(a);
        if (cfg->field[ETH_GW][0]) {
            g_object_set(G_OBJECT(s_ip4), NM_SETTING_IP_CONFIG_GATEWAY, cfg->field[ETH_GW], NULL);
        }
        if (cfg->field[ETH_DNS][0]) {
            nm_setting_ip_config_add_dns(s_ip4, cfg->field[ETH_DNS]);
        }
    }
    nm_connection_add_setting(con, NM_SETTING(s_ip4));

    /* No IPv6, as WiFi */
    g_object_set(G_OBJECT(s_ip6), NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_DISABLED, NULL);
    nm_connection_add_setting(con, NM_SETTING(s_ip6));
    return con;
}

static void eth_error(const char *what, GError *error) {
    LV_LOG_ERROR("Ethernet %s: %s", what, error->message);
    msg_update_text_fmt("Ethernet: %s", error->message);
    g_error_free(error);
}

static void eth_activated_cb(GObject *obj, GAsyncResult *result, gpointer user_data) {
    GError             *error = NULL;
    NMActiveConnection *ac = nm_client_activate_connection_finish(NM_CLIENT(obj), result, &error);

    if (ac) {
        g_object_unref(ac);
    } else {
        eth_error("activation", error);
    }
}

/* Saved: on the adapter now, if there is one. Not add-and-activate: it would tie the profile to this adapter's name */
static void eth_activate(NMRemoteConnection *rc) {
    NMDevice *d = eth_device();

    if (d) {
        nm_client_activate_connection_async(client, NM_CONNECTION(rc), d, NULL, NULL, eth_activated_cb, NULL);
    }
}

static void eth_added_cb(GObject *obj, GAsyncResult *result, gpointer user_data) {
    GError             *error = NULL;
    NMRemoteConnection *rc = nm_client_add_connection_finish(NM_CLIENT(obj), result, &error);

    if (rc) {
        eth_activate(rc);
        g_object_unref(rc);
    } else {
        eth_error("save", error);
    }
}

static void eth_saved_cb(GObject *obj, GAsyncResult *result, gpointer user_data) {
    GError *error = NULL;

    if (nm_remote_connection_commit_changes_finish(NM_REMOTE_CONNECTION(obj), result, &error)) {
        eth_activate(NM_REMOTE_CONNECTION(obj));
    } else {
        eth_error("save", error);
    }
}

void eth_apply(const eth_cfg_t *cfg) {
    if (!client) {
        msg_update_text_fmt("NetworkManager is not running");
        return;
    }
    NMRemoteConnection *rc = nm_client_get_connection_by_id(client, ETH_CON_ID);

    if (rc) {
        NMConnection *con = eth_connection_new(cfg, nm_connection_get_uuid(NM_CONNECTION(rc)));

        nm_connection_replace_settings_from_connection(NM_CONNECTION(rc), con);
        g_object_unref(con);
        nm_remote_connection_commit_changes_async(rc, TRUE, NULL, eth_saved_cb, NULL);
        return;
    }
    char         *uuid = nm_utils_uuid_generate();
    NMConnection *con = eth_connection_new(cfg, uuid);

    g_free(uuid);
    nm_client_add_connection_async(client, con, TRUE, NULL, eth_added_cb, NULL);
    g_object_unref(con);
}
