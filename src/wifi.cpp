/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */


#include "wifi.h"

extern "C" {

#include "btaudio.h"             /* sh_run: the WiFi log after a failed join */
#include "msg.h"
#include "params/params.h"
#include "pubsub_ids.h"

#include "radio.h"
#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

}

#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <array>

#define WLAN_IFACE "wlan0"
#define EMPTY_SSID_STR "--"

static GMainLoop *loop;
static NMClient  *client = NULL;
static NMDevice  *device = NULL;

static lv_timer_t *loop_timer;
static lv_timer_t *scan_timer = NULL;
/* The join asked for by hand (a press of Connect): its profile, for the retries and the reason on the screen */
static char        retry_uuid[40];
static char        retry_ssid[33];
static int         retry_n;
static gint64      retry_us;          /* when the last try started */

#define RETRY_TRIES     4
#define RETRY_WINDOW_US (120 * G_USEC_PER_SEC)   /* a failure later than this is not that join's */

/* An SSID for the message box, whose label colours "#rrggbb text#": a '#' in the name hid characters */
static const char *shown(const char *ssid) {
    static char buf[40];
    size_t      i = 0;

    for (; ssid[i] && i + 1 < sizeof(buf); i++) buf[i] = ssid[i] == '#' ? '-' : ssid[i];
    buf[i] = '\0';
    return buf;
}

static void retry_start(const char *uuid, const char *ssid) {
    snprintf(retry_uuid, sizeof(retry_uuid), "%s", uuid);
    snprintf(retry_ssid, sizeof(retry_ssid), "%s", ssid);
    retry_n = 0;
    retry_us = g_get_monotonic_time();
}

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
    radio_wifi_power(false);
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
    radio_wifi_power(true);
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

bool net_connected(void) {
    return eth_holds_wifi || status == WIFI_CONNECTED;
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
            wifi_ap_info_t ap_info = {};
            NMAccessPoint *ap = NM_ACCESS_POINT(g_ptr_array_index(aps, i));
            fill_access_point_info(active_ssid, ap, &ap_info);

            std::string ssid = ap_info.ssid;

            /* One row per network, the strongest of its access points: [1] is its place in ap_info_vec (it
             * was set to the access point's index in NM's list, which wrote over another network's row or
             * past the end with three or more access points of one network). Hidden ones each get a row */
            auto search = ssid_strength_map.find(ssid);
            if ((search != ssid_strength_map.end()) && (ssid.compare(EMPTY_SSID_STR) !=0 )) {
                if (search->second[0] < ap_info.strength) {
                    ap_info_vec[search->second[1]] = ap_info;
                    search->second[0] = ap_info.strength;
                }

            } else {
                ssid_strength_map[ssid] = {
                    {ap_info.strength, (uint)ap_info_vec.size()}
                };
                ap_info_vec.push_back(ap_info);
            }
        }

        aps_info.count = (uint8_t)std::min<size_t>(ap_info_vec.size(), 255);
        aps_info.ap_arr = (wifi_ap_info_t *)malloc(sizeof(wifi_ap_info_t) * std::max<size_t>(aps_info.count, 1));
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

/* The strongest access point of the network now, NULL when none is in the list */
static NMAccessPoint *find_ap(const uint8_t *ssid, size_t len) {
    const GPtrArray *aps = device ? nm_device_wifi_get_access_points(NM_DEVICE_WIFI(device)) : NULL;
    NMAccessPoint   *best = NULL;

    for (guint i = 0; aps && i < aps->len; i++) {
        NMAccessPoint *ap = NM_ACCESS_POINT(g_ptr_array_index(aps, i));
        GBytes        *s = nm_access_point_get_ssid(ap);
        gsize          n = 0;
        const void    *d = s ? g_bytes_get_data(s, &n) : NULL;

        if (d && n == len && memcmp(d, ssid, len) == 0
            && (!best || nm_access_point_get_strength(ap) > nm_access_point_get_strength(best))) {
            best = ap;
        }
    }
    return best;
}

const char *wifi_ap_refusal(const wifi_ap_info_t *ap) {
    if (ap->unsupported) {
        static char buf[160];

        snprintf(buf, sizeof(buf), "%s uses %s: the radio joins open and WPA2 networks", shown(ap->ssid), ap->unsupported);
        return buf;
    }
    if (ap->wpa3_only) {
        return "WPA3-only network: set the router to WPA2 or WPA2/WPA3";
    }
    if (ap->ssid_len == 0) {
        return "A hidden network: not supported";
    }
    return NULL;
}

void wifi_add_connection(const wifi_ap_info_t *ap_info, const char *password) {
    NMAccessPoint *ap = find_ap(ap_info->ssid_raw, ap_info->ssid_len);

    if (!ap) {
        msg_update_text_fmt("WiFi %s: no longer in range, scan again", shown(ap_info->ssid));
        return;
    }
    const char *ssid = ap_info->ssid;
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
    retry_start(uuid, ssid);
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
    g_ssid = g_bytes_new(ap_info->ssid_raw, ap_info->ssid_len);   /* the bytes, not the UTF-8 shown */
    s_wireless = (NMSettingWireless *)nm_setting_wireless_new();
    // clang-format off
    g_object_set(G_OBJECT(s_wireless),
				 NM_SETTING_WIRELESS_SSID, g_ssid,
				 NM_SETTING_WIRELESS_MODE, NM_SETTING_WIRELESS_MODE_INFRA,
				 NULL);
    // clang-format on
    g_bytes_unref(g_ssid);
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

    /* On the access point the list showed: NetworkManager otherwise looked for one itself, and with none that
     * fits (a security other than the profile's) it saved the profile as a hidden network, "known" from then on */
    LV_LOG_USER("Adding connection to %s (%s)", ssid, nm_access_point_get_bssid(ap));
    msg_update_text_fmt("Connecting to %s...", shown(ssid));
    nm_client_add_and_activate_connection_async(client, connection, device, nm_object_get_path(NM_OBJECT(ap)), NULL,
                                                connection_adding_and_activating_cb, NULL);
    g_object_unref(connection);
}

void wifi_update_connection(const char *uuid, const char *password) {
    NMRemoteConnection        *rem_con = NULL;
    NMConnection              *new_connection;
    NMSettingWirelessSecurity *s_wsec;
    gboolean                   temporary = FALSE;

    rem_con = nm_client_get_connection_by_uuid(client, uuid);
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
        /* user_data: join it again once saved ("Change password" saved and never reconnected) */
        nm_remote_connection_commit_changes_async(rem_con, !temporary, NULL, connection_modify_cb, (gpointer)1);
        g_object_unref(new_connection);
    } else {
        LV_LOG_ERROR("Connection %s not found", uuid);
    }
}

void wifi_delete_connection(const char *uuid) {
    NMRemoteConnection *rem_con = nm_client_get_connection_by_uuid(client, uuid);

    if (rem_con) {
        nm_remote_connection_delete_async(rem_con, NULL, connection_delete_cb, NULL);
    }
}

void wifi_connect(const char *uuid) {
    NMRemoteConnection *rem_con;

    if (device != NULL) {
        rem_con = nm_client_get_connection_by_uuid(client, uuid);
        if (rem_con != NULL) {
            retry_start(uuid, nm_connection_get_id(NM_CONNECTION(rem_con)));
            msg_update_text_fmt("Connecting to %s...", shown(retry_ssid));
            nm_client_activate_connection_async(client, NM_CONNECTION(rem_con), device, NULL, NULL,
                                                connection_activating_cb, loop);
        } else {
            LV_LOG_WARN("Connection %s is not found", uuid);
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
    guint32     flags, wpa_flags, rsn_flags;
    GBytes     *ssid;
    const char *hwaddr;
    char       *ssid_str;

    memset(ap_info, 0, sizeof(*ap_info));
    flags = nm_access_point_get_flags(ap);
    wpa_flags = nm_access_point_get_wpa_flags(ap);
    rsn_flags = nm_access_point_get_rsn_flags(ap);
    ssid = nm_access_point_get_ssid(ap);
    hwaddr = nm_access_point_get_bssid(ap);

    gsize       ssid_len = 0;
    const void *ssid_data = ssid ? g_bytes_get_data(ssid, &ssid_len) : NULL;

    if (ssid_data && ssid_len > 0 && ssid_len <= sizeof(ap_info->ssid_raw)) {
        memcpy(ap_info->ssid_raw, ssid_data, ssid_len);
        ap_info->ssid_len = (uint8_t)ssid_len;
        ssid_str = nm_utils_ssid_to_utf8((const guint8 *)ssid_data, ssid_len);
    } else {
        ssid_str = g_strdup(EMPTY_SSID_STR);
    }
    snprintf(ap_info->bssid, sizeof(ap_info->bssid), "%s", hwaddr ? hwaddr : "");
    snprintf(ap_info->ssid, sizeof(ap_info->ssid), "%s", ssid_str);
    ap_info->strength = nm_access_point_get_strength(ap);
    ap_info->mode = nm_access_point_get_mode(ap);
    ap_info->is_connected = active_ssid && ssid && (g_bytes_compare(ssid, active_ssid) == 0);
    g_free(ssid_str);

    /* Known: a saved profile NetworkManager would use on this access point (its SSID and a security that fits),
     * the latest used if several; not one merely named after the SSID */
    GPtrArray *fits = nm_access_point_filter_connections(ap, nm_client_get_connections(client));
    guint64    latest = 0;

    for (guint i = 0; fits && i < fits->len; i++) {
        NMConnection        *c = NM_CONNECTION(g_ptr_array_index(fits, i));
        NMSettingConnection *sc = nm_connection_get_setting_connection(c);
        guint64              ts = sc ? nm_setting_connection_get_timestamp(sc) : 0;

        if (!ap_info->known || ts > latest) {
            snprintf(ap_info->uuid, sizeof(ap_info->uuid), "%s", nm_connection_get_uuid(c));
            ap_info->known = true;
            latest = ts;
        }
    }
    if (fits) {
        g_ptr_array_unref(fits);
    }

    /* What it takes. The validator was set only for WPA2-PSK and for flags exactly NONE, and ap_info was the
     * last network's memory: an open network that also says WPS (flags not NONE), or a WPA1 one, kept the
     * previous row's validator, and an open network was joined as WPA-PSK with a password it does not have */
    guint32 akm     = wpa_flags | rsn_flags;
    bool    privacy = flags & NM_802_11_AP_FLAGS_PRIVACY;
    bool    psk     = akm & NM_802_11_AP_SEC_KEY_MGMT_PSK;
    bool    sae     = rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_SAE;
    bool    eap     = akm & NM_802_11_AP_SEC_KEY_MGMT_802_1X;
    bool    owe     = rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_OWE;

    if (psk || sae) {
        ap_info->password_validator = nm_utils_wpa_psk_valid;
        /* WPA3 only (SAE without WPA2-PSK): the window says so instead of asking a password */
        ap_info->wpa3_only = sae && !psk;
    } else if (eap) {
        ap_info->unsupported = "WPA Enterprise (802.1X)";
    } else if (owe) {
        ap_info->unsupported = "Enhanced Open (OWE)";
    } else if (privacy) {
        ap_info->unsupported = "WEP";
    }
    /* else open: no validator */
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
        LV_LOG_ERROR("Error adding and activating connection: %s (code: %d, domain: %u)", error->message, error->code,
                     error->domain);
        msg_update_text_fmt("WiFi: %s", error->message);
        retry_uuid[0] = '\0';
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
        msg_update_text_fmt("WiFi: %s", error->message);
        g_error_free(error);
    } else {
        LV_LOG_USER(("Connection '%s' (%s) successfully modified.\n"), nm_connection_get_id(NM_CONNECTION(connection)),
                    nm_connection_get_uuid(NM_CONNECTION(connection)));
        if (user_data) {
            wifi_connect(nm_connection_get_uuid(NM_CONNECTION(connection)));   /* the new password tried at once */
        }
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

/* Why a join failed, in words. On a secured network NetworkManager ends any join that takes too long as "no
 * secrets" (it has no agent to ask for a password again), so that reason covers a weak signal and a WPA3 or
 * PMF mismatch as well as a wrong password */
static const char *fail_text(guint reason) {
    switch (reason) {
        case NM_DEVICE_STATE_REASON_NO_SECRETS:            return "handshake failed: password, signal or router security";
        case NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT:    return "no answer: signal, channel or router security";
        case NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT: return "dropped while joining: signal or router security";
        case NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED:
        case NM_DEVICE_STATE_REASON_SUPPLICANT_CONFIG_FAILED: return "the WiFi service refused the profile";
        case NM_DEVICE_STATE_REASON_SSID_NOT_FOUND:        return "not in range";
        case NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE:
        case NM_DEVICE_STATE_REASON_IP_CONFIG_EXPIRED:
        case NM_DEVICE_STATE_REASON_DHCP_START_FAILED:
        case NM_DEVICE_STATE_REASON_DHCP_ERROR:
        case NM_DEVICE_STATE_REASON_DHCP_FAILED:           return "joined, but no address from DHCP";
        default:                                           return NULL;
    }
}

/* Worth another try: the join itself did not complete (the chip shares its antenna with Bluetooth, the
 * signal is weak) */
static bool fail_retry(guint reason) {
    return reason == NM_DEVICE_STATE_REASON_NO_SECRETS || reason == NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT
        || reason == NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT || reason == NM_DEVICE_STATE_REASON_SSID_NOT_FOUND;
}

static void device_state_changed_sig_cb(NMDevice *device, guint new_state, guint old_state, guint reason,
                                        gpointer user_data) {
    LV_LOG_USER("WiFi state %u -> %u, reason %u", old_state, new_state, reason);
    switch (new_state) {
    case NM_DEVICE_STATE_FAILED: {
        /* The join asked for by hand, if this is its failure (the active connection's profile, or within its
         * time when NetworkManager no longer says) */
        NMActiveConnection *ac = nm_device_get_active_connection(device);
        const char         *failed = ac ? nm_active_connection_get_uuid(ac) : NULL;
        bool                ours = retry_uuid[0]
            && (failed ? strcmp(failed, retry_uuid) == 0 : g_get_monotonic_time() - retry_us < RETRY_WINDOW_US);
        const char         *why = fail_text(reason);

        if (!ours) {
            LV_LOG_WARN("WiFi join failed (reason %u%s%s)", reason, why ? ": " : "", why ? why : "");
            break;
        }
        NMRemoteConnection *rc = nm_client_get_connection_by_uuid(client, retry_uuid);

        if (rc && fail_retry(reason) && retry_n < RETRY_TRIES - 1) {
            retry_n++;
            retry_us = g_get_monotonic_time();
            LV_LOG_WARN("WiFi %s: %s (reason %u), try %d/%d", retry_ssid, why ? why : "failed", reason, retry_n + 1,
                        RETRY_TRIES);
            msg_update_text_fmt("Connecting to %s... (try %d/%d)", shown(retry_ssid), retry_n + 1, RETRY_TRIES);
            nm_client_activate_connection_async(client, NM_CONNECTION(rc), device, NULL, NULL,
                                                connection_activating_cb, loop);
        } else {
            LV_LOG_WARN("WiFi %s: %s (reason %u)", retry_ssid, why ? why : "failed", reason);
            if (why) {
                msg_update_text_fmt("WiFi %s: %s", shown(retry_ssid), why);
            } else {
                msg_update_text_fmt("WiFi %s: join failed (reason %u)", shown(retry_ssid), reason);
            }
            retry_uuid[0] = '\0';
            retry_n = 0;
            /* What the driver, NetworkManager and the supplicant said, for a report: /mnt/app_logs/wifi.txt */
            FILE *f = fopen("/tmp/x6100-wifilog.ssid", "w");
            if (f) {
                fputs(retry_ssid, f);
                fclose(f);
            }
            sh_run("/usr/sbin/x6100-wifilog", NULL, NULL);
        }
        break;
    }
    case NM_DEVICE_STATE_ACTIVATED:
        retry_uuid[0] = '\0';
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
    LV_LOG_USER("WiFi connection state %u, reason %u", state, reason);
    lv_msg_send(MSG_WIFI_STATE_CHANGED, NULL);
}

static void connection_activating_cb(GObject *client, GAsyncResult *result, gpointer user_data) {
    NMActiveConnection *active_con;
    GError             *error = NULL;

    active_con = nm_client_activate_connection_finish(NM_CLIENT(client), result, &error);

    if (error) {
        LV_LOG_ERROR("Error activating connection: %s (code: %d, domain: %u)", error->message, error->code,
                     error->domain);
        msg_update_text_fmt("WiFi: %s", error->message);
        retry_uuid[0] = '\0';
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
