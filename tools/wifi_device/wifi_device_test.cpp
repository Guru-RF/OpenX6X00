/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the WiFi device coming and going (src/wifi.cpp): the WiFi/BT chip powered off and on by
 *  the switch, its device dropped by NetworkManager under the GUI (the chip's USB link reset), and the switch flipped
 *  twice in a moment.
 *
 *  The real wifi.cpp on the PC build's LVGL, with GLib's real objects and signals. NetworkManager is not asked for
 *  anything: the client and the devices are objects of the test's own types, with the signals wifi.cpp connects to
 *  ("device-added" and "device-removed" on the client, "state-changed" on a device), and the libnm calls wifi.cpp
 *  makes on them are the test's (a symbol of the program goes before the library's). Each of those calls notes a
 *  device that is no more: a freed device read is counted, not a crash by luck. What NetworkManager does is played
 *  by the test: it holds one reference to each device, emits device-removed and drops the reference when the
 *  interface goes, makes a new device and emits device-added when it comes.
 *
 *  Checked:
 *  - at the start the device is taken with a reference of our own
 *  - the device goes while WiFi is on (unmanaged, removed, freed by NetworkManager): the status is Starting, not
 *    Connected, and the scan, the access point list, the address, the scan timer and the "no network" check read no
 *    freed device (before: all of them did; the list read a NULL array too)
 *  - it comes again: taken, Disconnected, then Connected as it joins
 *  - the device unavailable (the chip stopped under it): Starting; back: Disconnected
 *  - off: the power pin cut, the status Off, what the device says while it goes not ours (before: its last
 *    "disconnected" showed with the switch off), our reference dropped
 *  - a device that comes while WiFi is off is not taken (before: Disconnected with the switch off)
 *  - on: the pin on, the device NetworkManager still has taken at once; device-added for that same device after it
 *    connects no second set of handlers
 *  - a quick off and on, the chip's old device still there at on, going after, the new one coming: ends Connected
 *  - a quick on and off, the chip's device coming after the off: ends Off, the pin cut
 *  - wlan0 coming while a device of another name is held (#3) takes over; another name while wlan0 is held does not
 *  See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "wifi.h"

extern "C" {
#include "lvgl.h"
#include "params/params.h"
#include "radio.h"
}

#include <glib-object.h>
#include <NetworkManager.h>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <vector>

/* ---- What wifi.cpp calls of the rest of the GUI, stood in for ---- */

extern "C" {

params_t params;
uint32_t lv_tick_mono_now;      /* LVGL's clock (lv_tick_mono.h), stepped by run() */

static bool pin_off;            /* the chip's power pin as the GUI set it: radio_wifi_power(true) cuts the power */
static int  pin_sets;

void radio_wifi_power(bool on) {
    pin_off = on;
    pin_sets++;
}

void params_bool_set(params_bool_t *var, bool x) {
    var->x = x;
}

void msg_update_text_fmt(const char *fmt, ...) {
    (void)fmt;
}

int sh_run(const char *cmd, char *out, size_t size) {
    (void)cmd, (void)out, (void)size;
    return 0;
}

void buttons_refresh(void) {
}

}   /* extern "C" */

/* ---- NetworkManager, played by the test ---- */

static GType           dev_type, client_type;
static guint           sig_state, sig_added, sig_removed;
static GObject        *the_client;
static std::vector<GObject *> nm_devs;      /* the devices NetworkManager has, one reference each */
static std::set<gpointer>     live;         /* every device not freed yet */
static int             freed_reads;         /* libnm calls made on a freed device */

static void dev_freed(gpointer data, GObject *where) {
    (void)data;
    live.erase(where);
}

static bool alive(gconstpointer d) {
    if (live.count((gpointer)d)) {
        return true;
    }
    freed_reads++;
    return false;
}

static GObject *nm_new_device(const char *iface, guint state) {
    GObject *d = (GObject *)g_object_new(dev_type, NULL);

    g_object_set_data_full(d, "iface", g_strdup(iface), g_free);
    g_object_set_data(d, "state", GUINT_TO_POINTER(state));
    g_object_weak_ref(d, dev_freed, NULL);
    live.insert(d);
    nm_devs.push_back(d);
    return d;
}

/* The interface there: a new device, device-added */
static GObject *nm_device_comes(const char *iface, guint state) {
    GObject *d = nm_new_device(iface, state);

    g_signal_emit(the_client, sig_added, 0, d);
    return d;
}

static void nm_state(GObject *d, guint state) {
    guint old = GPOINTER_TO_UINT(g_object_get_data(d, "state"));

    g_object_set_data(d, "state", GUINT_TO_POINTER(state));
    g_signal_emit(d, sig_state, 0, state, old, 0u);
}

/* The interface gone: unmanaged, device-removed, NetworkManager's reference dropped */
static void nm_device_goes(GObject *d) {
    nm_state(d, NM_DEVICE_STATE_UNMANAGED);
    for (size_t i = 0; i < nm_devs.size(); i++) {
        if (nm_devs[i] == d) nm_devs.erase(nm_devs.begin() + i);
    }
    g_signal_emit(the_client, sig_removed, 0, d);
    g_object_unref(d);
}

/* How many times wifi.cpp is connected to the device's state-changed */
static guint handlers(GObject *d) {
    guint n = g_signal_handlers_block_matched(d, G_SIGNAL_MATCH_ID, sig_state, 0, NULL, NULL, NULL);

    g_signal_handlers_unblock_matched(d, G_SIGNAL_MATCH_ID, sig_state, 0, NULL, NULL, NULL);
    return n;
}

extern "C" {

GType nm_client_get_type(void) { return client_type; }
GType nm_device_get_type(void) { return dev_type; }
GType nm_device_wifi_get_type(void) { return dev_type; }

NMClient *nm_client_new(GCancellable *c, GError **error) {
    (void)c, (void)error;
    return (NMClient *)the_client;
}

const GPtrArray *nm_client_get_devices(NMClient *client) {
    static GPtrArray *arr;

    (void)client;
    if (!arr) arr = g_ptr_array_new();
    g_ptr_array_set_size(arr, 0);
    for (GObject *d : nm_devs) g_ptr_array_add(arr, d);
    return arr;
}

NMDevice *nm_client_get_device_by_iface(NMClient *client, const char *iface) {
    (void)client;
    for (GObject *d : nm_devs) {
        if (g_strcmp0((const char *)g_object_get_data(d, "iface"), iface) == 0) return (NMDevice *)d;
    }
    return NULL;
}

const GPtrArray *nm_client_get_connections(NMClient *client) {
    static GPtrArray *none;

    (void)client;
    if (!none) none = g_ptr_array_new();
    return none;
}

NMRemoteConnection *nm_client_get_connection_by_id(NMClient *client, const char *id) {
    (void)client, (void)id;
    return NULL;
}

void nm_client_dbus_set_property(NMClient *client, const char *path, const char *iface, const char *name, GVariant *v,
                                 int timeout, GCancellable *c, GAsyncReadyCallback cb, gpointer data) {
    (void)client, (void)path, (void)iface, (void)name, (void)timeout, (void)c, (void)cb, (void)data;
    g_variant_unref(g_variant_ref_sink(v));
}

const char *nm_device_get_iface(NMDevice *d) {
    return alive(d) ? (const char *)g_object_get_data(G_OBJECT(d), "iface") : "freed";
}

const char *nm_device_get_driver(NMDevice *d) {
    return alive(d) ? "rtl8723bu" : "freed";
}

NMDeviceState nm_device_get_state(NMDevice *d) {
    return alive(d) ? (NMDeviceState)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(d), "state")) : NM_DEVICE_STATE_UNKNOWN;
}

NMActiveConnection *nm_device_get_active_connection(NMDevice *d) {
    alive(d);
    return NULL;
}

NMIPConfig *nm_device_get_ip4_config(NMDevice *d) {
    alive(d);
    return NULL;
}

const GPtrArray *nm_device_wifi_get_access_points(NMDeviceWifi *d) {
    alive(d);
    return NULL;        /* as libnm gives for a device that is not one */
}

NMAccessPoint *nm_device_wifi_get_active_access_point(NMDeviceWifi *d) {
    alive(d);
    return NULL;
}

gint64 nm_device_wifi_get_last_scan(NMDeviceWifi *d) {
    alive(d);
    return 1;
}

void nm_device_wifi_request_scan_async(NMDeviceWifi *d, GCancellable *c, GAsyncReadyCallback cb, gpointer data) {
    (void)c, (void)cb, (void)data;
    alive(d);
}

}   /* extern "C" */

/* ---- The checks ---- */

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static const char *name(wifi_status_t s) {
    static const char *names[] = {"Off", "Starting", "Disconnected", "Connecting", "Connected"};

    return names[s];
}

/* The GUI loop for ms: LVGL's timers, wifi.cpp's among them (the scan status, the Ethernet look every second, the
 * "no network" check every minute) */
static void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
        lv_tick_mono_now += 100;
        lv_timer_handler();
    }
}

/* Whatever reads the WiFi device, as the WiFi window, the top bar and the timers do */
static void read_all(void) {
    char  ip[64], gw[64];
    char *ip_p = ip, *gw_p = gw;

    wifi_aps_info_delete(wifi_get_available_access_points());
    wifi_start_scan();
    wifi_get_ipaddr(&ip_p, &gw_p);
    wifi_get_status();
    run(61000);
}

int main(void) {
    lv_init();
    dev_type = g_type_register_static_simple(G_TYPE_OBJECT, "TestDevice", sizeof(GObjectClass), NULL, sizeof(GObject), NULL,
                                             (GTypeFlags)0);
    client_type = g_type_register_static_simple(G_TYPE_OBJECT, "TestClient", sizeof(GObjectClass), NULL, sizeof(GObject),
                                                NULL, (GTypeFlags)0);
    sig_state = g_signal_new("state-changed", dev_type, G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_UINT,
                             G_TYPE_UINT, G_TYPE_UINT);
    sig_added = g_signal_new("device-added", client_type, G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                             G_TYPE_OBJECT);
    sig_removed = g_signal_new("device-removed", client_type, G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                               G_TYPE_OBJECT);
    the_client = (GObject *)g_object_new(client_type, NULL);

    printf("The start, WiFi on, the chip up and joined\n");
    params.wifi_enabled.x = true;
    GObject *d = nm_new_device("wlan0", NM_DEVICE_STATE_ACTIVATED);

    wifi_power_setup();
    nm_state(d, NM_DEVICE_STATE_ACTIVATED);
    check(wifi_get_status() == WIFI_CONNECTED && !pin_off, "%s, the pin on", name(wifi_get_status()));
    check(d->ref_count == 2 && handlers(d) == 1, "the device held with a reference of our own (%u), one handler (%u)",
          d->ref_count, handlers(d));

    printf("The device goes under the GUI (the chip's USB link reset), WiFi on\n");
    nm_device_goes(d);
    check(!live.count(d), "NetworkManager's reference was the last: the device is freed");
    check(wifi_get_status() == WIFI_STARTING, "%s (not Connected)", name(wifi_get_status()));
    freed_reads = 0;
    read_all();
    check(freed_reads == 0, "the list, a scan, the address and a minute of the timers read no freed device (%d reads)",
          freed_reads);
    check(!wifi_scanning(), "no scan in progress");

    printf("It comes again\n");
    d = nm_device_comes("wlan0", NM_DEVICE_STATE_DISCONNECTED);
    check(wifi_get_status() == WIFI_DISCONNECTED && d->ref_count == 2 && handlers(d) == 1, "taken: %s",
          name(wifi_get_status()));
    nm_state(d, NM_DEVICE_STATE_ACTIVATED);
    check(wifi_get_status() == WIFI_CONNECTED, "joined: %s", name(wifi_get_status()));

    printf("The device unavailable, then back\n");
    nm_state(d, NM_DEVICE_STATE_UNAVAILABLE);
    check(wifi_get_status() == WIFI_STARTING, "%s", name(wifi_get_status()));
    nm_state(d, NM_DEVICE_STATE_DISCONNECTED);
    check(wifi_get_status() == WIFI_DISCONNECTED, "%s", name(wifi_get_status()));
    nm_state(d, NM_DEVICE_STATE_ACTIVATED);

    printf("Off\n");
    wifi_power_off();
    check(wifi_get_status() == WIFI_OFF && pin_off && !params.wifi_enabled.x, "%s, the pin cut, the setting off",
          name(wifi_get_status()));
    check(d->ref_count == 1 && handlers(d) == 0, "our reference and handler dropped (%u, %u)", d->ref_count, handlers(d));
    nm_state(d, NM_DEVICE_STATE_DISCONNECTED);      /* what the device says as the chip loses its power */
    check(wifi_get_status() == WIFI_OFF, "the device's \"disconnected\" as it goes: still %s", name(wifi_get_status()));
    nm_device_goes(d);
    freed_reads = 0;
    read_all();
    check(wifi_get_status() == WIFI_OFF && freed_reads == 0, "gone: %s, no freed device read", name(wifi_get_status()));

    printf("A device that comes while WiFi is off\n");
    d = nm_device_comes("wlan0", NM_DEVICE_STATE_DISCONNECTED);
    check(wifi_get_status() == WIFI_OFF && d->ref_count == 1 && handlers(d) == 0, "not taken: %s", name(wifi_get_status()));

    printf("On, the device NetworkManager has still there\n");
    wifi_power_on();
    check(!pin_off && params.wifi_enabled.x, "the pin on, the setting on");
    check(wifi_get_status() == WIFI_DISCONNECTED && d->ref_count == 2 && handlers(d) == 1, "taken at once: %s",
          name(wifi_get_status()));
    g_signal_emit(the_client, sig_added, 0, d);
    check(handlers(d) == 1 && d->ref_count == 2, "device-added for the same device: still one handler, one reference");
    nm_state(d, NM_DEVICE_STATE_ACTIVATED);
    check(wifi_get_status() == WIFI_CONNECTED, "joined: %s", name(wifi_get_status()));

    printf("A quick off and on: the old device still there at on, going after, the chip's new one coming\n");
    wifi_power_off();
    wifi_power_on();
    check(!pin_off && params.wifi_enabled.x, "the pin on, the setting on");
    nm_device_goes(d);
    check(wifi_get_status() == WIFI_STARTING, "the old device gone: %s", name(wifi_get_status()));
    freed_reads = 0;
    read_all();
    check(freed_reads == 0, "no freed device read");
    d = nm_device_comes("wlan0", NM_DEVICE_STATE_DISCONNECTED);
    nm_state(d, NM_DEVICE_STATE_ACTIVATED);
    check(wifi_get_status() == WIFI_CONNECTED && handlers(d) == 1, "the new one taken and joined: %s",
          name(wifi_get_status()));

    printf("A quick on and off: off, on, off, the chip's device coming after\n");
    wifi_power_off();
    nm_device_goes(d);
    wifi_power_on();
    check(wifi_get_status() == WIFI_STARTING, "on, no device yet: %s", name(wifi_get_status()));
    wifi_power_off();
    d = nm_device_comes("wlan0", NM_DEVICE_STATE_DISCONNECTED);
    nm_state(d, NM_DEVICE_STATE_DISCONNECTED);
    freed_reads = 0;
    read_all();
    check(wifi_get_status() == WIFI_OFF && pin_off && !params.wifi_enabled.x && handlers(d) == 0,
          "ends %s, the pin cut, the device not taken", name(wifi_get_status()));
    nm_device_goes(d);

    printf("wlan0 and a device of another name (#3)\n");
    wifi_power_on();
    GObject *other = nm_device_comes("wlan1", NM_DEVICE_STATE_DISCONNECTED);

    check(handlers(other) == 1 && wifi_get_status() == WIFI_DISCONNECTED, "wlan1 alone is taken: %s",
          name(wifi_get_status()));
    d = nm_device_comes("wlan0", NM_DEVICE_STATE_DISCONNECTED);
    check(handlers(d) == 1 && handlers(other) == 0 && other->ref_count == 1, "wlan0 comes: taken, wlan1 let go");
    GObject *third = nm_device_comes("wlan2", NM_DEVICE_STATE_DISCONNECTED);

    check(handlers(d) == 1 && handlers(third) == 0, "wlan2 comes: wlan0 kept");
    nm_device_goes(other);
    check(wifi_get_status() == WIFI_DISCONNECTED && handlers(d) == 1, "wlan1 goes: nothing changes (%s)",
          name(wifi_get_status()));
    freed_reads = 0;
    read_all();
    check(freed_reads == 0, "no freed device read");

    printf("%d of %d cases ok\n", good, cases);
    return good == cases ? 0 : 1;
}
