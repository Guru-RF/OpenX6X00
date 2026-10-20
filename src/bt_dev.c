/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Bluetooth devices straight to BlueZ over D-Bus, instead of driving
 *  bluetoothctl through a pipe:
 *
 *    list     ObjectManager.GetManagedObjects, Device1 properties
 *    scan     Adapter1.StartDiscovery / StopDiscovery, LE and classic in turns
 *    pair     Device1.Pair, then Trusted = true, then Device1.Connect
 *    connect  Device1.Connect           disconnect  Device1.Disconnect
 *    forget   Adapter1.RemoveDevice
 *
 *  Each step gets BlueZ's own reply or error name, not text guessed from a
 *  terminal. Ported from openx6100's bt_dev.c: D-Bus backend and agent by
 *  SP2KMO, alternating scan, keyboards found again at a new address and
 *  recovery after WiFi off/on by randal007.
 *
 *  Differences from upstream: "one device at a time" only for headphones
 *  (connecting a pair disconnects the others; a CAT phone and a keyboard stay,
 *  and nothing is Blocked, so the startup list still works); the pairing
 *  window is btcat-ctl's (/tmp/btcat-pairing, the Pairing switch in
 *  Settings > Bluetooth), and the log is /tmp/bt_pair.log.
 *  From upstream's bt-security work (3b8fd331): the window enforced on every
 *  refresh, the phone tag, the adapter state for the status row.
 *
 *  Our agent, on its own thread and D-Bus connection, is the only one and
 *  the default one. It answers pairing questions only while the window is
 *  open:
 *    RequestPinCode       "0000" (older headsets), a random code to type
 *                         for a keyboard
 *    DisplayPasskey/PinCode  shown in bt_dev_activity(): type it on the keyboard
 *    RequestPasskey       0 (nothing to type it on)
 *    RequestConfirmation, RequestAuthorization  accepted (no keypad)
 *  AuthorizeService is accepted for a paired device at any time. It
 *  registers again whenever bluetoothd (re)appears on the bus, and asks to
 *  be the default agent every 30 s.
 *
 *  The worker's D-Bus calls are synchronous on its own connection; the GUI
 *  only reads the cached list.
 */

#include "bt_dev.h"

#include <gio/gio.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SCAN_LE_S       3       /* Scan: LE turn (keyboards) ...           */
#define SCAN_BREDR_S    5       /* ... then classic (headphones) ...       */
#define SCAN_ROUNDS     2       /* ... twice: 16 s, see do_scan()          */
#define PAIR_FIND_S     30      /* wait this long for a new device to show up */
#define PAIR_TIMEOUT_MS 60000
#define CONN_TIMEOUT_MS 30000
#define CALL_TIMEOUT_MS 10000
#define AGENT_TICK_S    30

#define BLUEZ           "org.bluez"
#define ADAPTER_PATH    "/org/bluez/hci0"
#define AGENT_PATH      "/org/x6100/bt_agent"
#define AGENT_CAP       "NoInputNoOutput"
#define AGENT_CAP_KBD   "KeyboardDisplay"   /* only while pairing a keyboard */
#define AGENT_PIN       "0000"

#define BTCAT_CTL       "/usr/sbin/btcat-ctl"
#define BTAUDIO_CTL     "/usr/sbin/btaudio-ctl"
#define PAIR_WIN        "/tmp/btcat-pairing"    /* btcat-ctl: window end, s of uptime */
#define PAIR_LOG        "/tmp/bt_pair.log"
#define PAIR_LOG_MAX    65536

typedef enum {
    CMD_NONE = 0,
    CMD_REFRESH,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_SCAN,
    CMD_FORGET,
} cmd_t;

static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;

    cmd_t           cmd;
    char            cmd_addr[18];
    bool            busy;

    bt_dev_t        list[BT_DEV_MAX];
    int             count;
    bt_adapter_t    adapter;
    bool            scanning;
    char            activity[48];
    char            agent_cap[20];

    GDBusConnection *conn;      /* worker thread only */
} S = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static struct {
    GMainContext    *ctx;
    GDBusConnection *conn;      /* agent thread */
} A;

static void agent_set_cap(const char *cap);

/* ---- Log ------------------------------------------------------------ */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void plog(const char *fmt, ...) {
    struct stat st;
    struct tm   tm;
    time_t      now = time(NULL);
    char        ts[24];
    va_list     ap;
    FILE       *f;

    pthread_mutex_lock(&log_lock);
    f = fopen(PAIR_LOG, (stat(PAIR_LOG, &st) == 0 && st.st_size > PAIR_LOG_MAX) ? "w" : "a");
    if (f) {
        localtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(f, "%s ", ts);
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    pthread_mutex_unlock(&log_lock);
}

/* ---- Helpers ---------------------------------------------------------- */

static void set_activity(const char *what) {
    pthread_mutex_lock(&S.lock);
    snprintf(S.activity, sizeof(S.activity), "%s", what);
    pthread_mutex_unlock(&S.lock);
}

static bool addr_ok(const char *addr) {
    if (!addr || strlen(addr) != 17) {
        return false;
    }
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2 ? addr[i] != ':' : !g_ascii_isxdigit(addr[i])) {
            return false;
        }
    }
    return true;
}

/* "AA:BB:..." -> "/org/bluez/hci0/dev_AA_BB_..." */
static void dev_path(const char *addr, char *out, size_t out_sz) {
    snprintf(out, out_sz, ADAPTER_PATH "/dev_%.17s", addr);
    for (char *p = out; *p; p++) {
        if (*p == ':') *p = '_';
    }
}

static GDBusConnection *bus_new(void) {
    GError          *err = NULL;
    GDBusConnection *c = NULL;
    gchar           *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &err);

    if (addr) {
        c = g_dbus_connection_new_for_address_sync(addr,
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                NULL, NULL, &err);
        g_free(addr);
    }
    if (!c) {
        plog("D-Bus: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
    return c;
}

/* Private system-bus connection of the worker, reopened if the bus went away */
static GDBusConnection *bus(void) {
    if (S.conn && g_dbus_connection_is_closed(S.conn)) {
        g_object_unref(S.conn);
        S.conn = NULL;
    }
    if (!S.conn) {
        S.conn = bus_new();
    }
    return S.conn;
}

/* Synchronous call on connection c. Returns the reply (unref it) or NULL;
 * on failure the BlueZ error name goes to errname (may be NULL). */
static GVariant *call_on(GDBusConnection *c, const char *path, const char *iface, const char *method,
                         GVariant *params, const GVariantType *reply_type, int timeout_ms,
                         char *errname, size_t errname_sz) {
    GError   *err = NULL;
    GVariant *r;

    if (errname && errname_sz) {
        errname[0] = '\0';
    }
    if (!c) {
        if (params) g_variant_unref(g_variant_ref_sink(params));
        return NULL;
    }
    r = g_dbus_connection_call_sync(c, BLUEZ, path, iface, method, params, reply_type,
                                    G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, &err);
    if (!r) {
        gchar *remote = g_dbus_error_get_remote_error(err);

        if (errname && errname_sz) {
            snprintf(errname, errname_sz, "%s", remote ? remote : "");
        }
        g_free(remote);
        /* list and property polls fail quietly while BT is off or a device is unknown */
        if (strcmp(method, "GetManagedObjects") != 0 && strcmp(method, "Get") != 0 && strcmp(method, "GetAll") != 0) {
            plog("%s %s: %s", method, path, err ? err->message : "?");
        }
        g_clear_error(&err);
    }
    return r;
}

static GVariant *call(const char *path, const char *iface, const char *method, GVariant *params,
                      const GVariantType *reply_type, int timeout_ms, char *errname, size_t errname_sz) {
    return call_on(bus(), path, iface, method, params, reply_type, timeout_ms, errname, errname_sz);
}

static void unref(GVariant *v) {
    if (v) g_variant_unref(v);
}

static bool set_prop(GDBusConnection *c, const char *path, const char *iface, const char *prop, bool val) {
    GVariant *r = call_on(c, path, "org.freedesktop.DBus.Properties", "Set",
                          g_variant_new("(ssv)", iface, prop, g_variant_new_boolean(val)),
                          NULL, CALL_TIMEOUT_MS, NULL, 0);
    unref(r);
    return r != NULL;
}

/* Boolean property; false when missing, and then *exists (if given) is false too */
static bool get_prop(GDBusConnection *c, const char *path, const char *iface, const char *prop, bool *exists) {
    GVariant *v = NULL;
    bool      val = false;
    GVariant *r = call_on(c, path, "org.freedesktop.DBus.Properties", "Get",
                          g_variant_new("(ss)", iface, prop), G_VARIANT_TYPE("(v)"), CALL_TIMEOUT_MS, NULL, 0);

    if (exists) *exists = r != NULL;
    if (!r) return false;
    g_variant_get(r, "(v)", &v);
    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
        val = g_variant_get_boolean(v);
    }
    unref(v);
    g_variant_unref(r);
    return val;
}

static bool dev_bool(const char *addr, const char *prop, bool *exists) {
    char path[64];

    dev_path(addr, path, sizeof(path));
    return get_prop(bus(), path, "org.bluez.Device1", prop, exists);
}

static bool device_known(const char *addr) {
    bool exists = false;

    dev_bool(addr, "Paired", &exists);
    return exists;
}

static GVariant *managed_objects(void) {
    return call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                G_VARIANT_TYPE("(a{oa{sa{sv}}})"), CALL_TIMEOUT_MS, NULL, 0);
}

/* Start or stop an inquiry; false only if it could not start. transport:
 * "auto", "le" or "bredr". Classic and LE together is what this chip
 * (RTL8723BU) does worst: during a classic inquiry it hears hardly any LE
 * advertising, none at all after BT was switched off and on with WiFi
 * running. So keyboards are looked for with "le", and Scan takes turns. */
static bool discovery(bool on, const char *transport) {
    char      en[96];
    GVariant *r;

    if (on) {
        GVariantBuilder b;

        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "Transport", g_variant_new_string(transport));
        unref(call(ADAPTER_PATH, "org.bluez.Adapter1", "SetDiscoveryFilter",
                   g_variant_new("(a{sv})", &b), NULL, CALL_TIMEOUT_MS, NULL, 0));
    }
    r = call(ADAPTER_PATH, "org.bluez.Adapter1", on ? "StartDiscovery" : "StopDiscovery", NULL, NULL,
             CALL_TIMEOUT_MS, en, sizeof(en));
    unref(r);
    /* "InProgress": another client's inquiry is running, fine too */
    return r || !on || strstr(en, "InProgress");
}

/* ---- Device kinds ------------------------------------------------------- */

static bool has_uuid(GVariant *dev, const char *prefix) {
    GVariant    *uuids = g_variant_lookup_value(dev, "UUIDs", G_VARIANT_TYPE("as"));
    GVariantIter it;
    const char  *u;
    bool         found = false;

    if (!uuids) return false;
    g_variant_iter_init(&it, uuids);
    while (!found && g_variant_iter_next(&it, "&s", &u)) {
        found = g_ascii_strncasecmp(u, prefix, 8) == 0;
    }
    g_variant_unref(uuids);
    return found;
}

/* Keyboard or other input device: Class major 0x05, HID Appearance, Icon
 * "input-..." or the HID services (0x1812 HID over GATT, 0x1124 classic HID).
 * dev: a Device1 property dictionary (a{sv}). */
static bool props_input(GVariant *dev) {
    guint32     cls = 0;
    guint16     appearance = 0;
    const char *icon = NULL;

    g_variant_lookup(dev, "Class", "u", &cls);
    g_variant_lookup(dev, "Appearance", "q", &appearance);
    g_variant_lookup(dev, "Icon", "&s", &icon);
    return ((cls >> 8) & 0x1F) == 0x05 || (appearance & 0xFFC0) == 0x03C0 ||
           (icon && strncmp(icon, "input-", 6) == 0) ||
           has_uuid(dev, "00001812") || has_uuid(dev, "00001124");
}

/* Headphones or speaker: Icon "audio-..." or an A2DP sink (0x110B) that is
 * not a phone (Handsfree Audio Gateway 0x111F) */
static bool props_audio(GVariant *dev) {
    const char *icon = NULL;

    g_variant_lookup(dev, "Icon", "&s", &icon);
    return (icon && strncmp(icon, "audio-", 6) == 0) ||
           (has_uuid(dev, "0000110b") && !has_uuid(dev, "0000111f"));
}

/* A phone (Class major 0x02 or Icon "phone"): a CAT client, it pairs and
 * connects from its own side (upstream's props_phone) */
static bool props_phone(GVariant *dev) {
    guint32     cls = 0;
    const char *icon = NULL;

    g_variant_lookup(dev, "Class", "u", &cls);
    g_variant_lookup(dev, "Icon", "&s", &icon);
    return ((cls >> 8) & 0x1F) == 0x02 || (icon && strcmp(icon, "phone") == 0);
}

/* Device1 properties of path over c (unref), or NULL */
static GVariant *dev_props(GDBusConnection *c, const char *path) {
    GVariant *props;
    GVariant *r = call_on(c, path, "org.freedesktop.DBus.Properties", "GetAll",
                          g_variant_new("(s)", "org.bluez.Device1"), G_VARIANT_TYPE("(a{sv})"),
                          CALL_TIMEOUT_MS, NULL, 0);

    if (!r) return NULL;
    props = g_variant_get_child_value(r, 0);
    g_variant_unref(r);
    return props;
}

static bool path_is_input(GDBusConnection *c, const char *path) {
    GVariant *props = path ? dev_props(c, path) : NULL;
    bool      input = props && props_input(props);

    unref(props);
    return input;
}

static bool path_is_paired(GDBusConnection *c, const char *path) {
    GVariant *props = path ? dev_props(c, path) : NULL;
    gboolean  paired = FALSE;

    if (props) g_variant_lookup(props, "Paired", "b", &paired);
    unref(props);
    return paired;
}

/* ---- Pairing window ------------------------------------------------------
 *
 * The radio has no screen for pairing questions, and an agent that accepts
 * everything all the time lets anyone in range pair. So pairing is open
 * only during btcat-ctl's window: the Pairing switch in Settings, or a
 * pairing started here. Paired devices connect at any time. */
int bt_dev_visible_s(void) {
    long            end = 0;
    struct timespec now;
    FILE           *f = fopen(PAIR_WIN, "r");

    if (f) {
        if (fscanf(f, "%ld", &end) != 1) end = 0;
        fclose(f);
    }
    clock_gettime(CLOCK_BOOTTIME, &now);
    return end > now.tv_sec ? (int)(end - now.tv_sec) : 0;
}

static bool pairing_is_open(void) {
    return bt_dev_visible_s() > 0;
}

/* Visible and pairable for 3 min, with the countdown in Settings */
static void pairing_open(void) {
    if (system(BTCAT_CTL " discoverable on >/dev/null 2>&1") != 0) {
        plog("btcat-ctl discoverable on failed");
    }
    plog("pairing open");
}

/* ---- Device list -------------------------------------------------------- */

/* Paired or connected devices are listed. Not paired: headphones with a name
 * and keyboards (what Scan is for); phones pair from their side in the
 * pairing window. Many cheap BT 3.0 keyboards never answer the remote-name
 * request, so a keyboard without a Name is listed as "Keyboard <addr>".
 * Fills d->name. */
static bool dev_listed(bt_dev_t *d, const char *addr, const char *name, bool paired) {
    if (!paired && !(d->input || (name && d->audio))) {
        return false;
    }
    if (name) {
        snprintf(d->name, sizeof(d->name), "%s", name);
    } else {
        snprintf(d->name, sizeof(d->name), d->input ? "Keyboard %s" : "%s", addr);
    }
    return true;
}

static void do_refresh(void) {
    bt_dev_t      list[BT_DEV_MAX];
    bt_dev_t      rest[BT_DEV_MAX];
    int           n = 0, nrest = 0;
    GVariantIter *objs;
    const char   *path;
    GVariant     *ifaces;
    GVariant     *r = managed_objects();
    bt_adapter_t  ad = { .seen = true, .bluez = r != NULL };

    if (r) {
        g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
        while (g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            GVariant   *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
            GVariant   *adp = strcmp(path, ADAPTER_PATH) == 0 ?
                              g_variant_lookup_value(ifaces, "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}")) : NULL;

            if (adp) {
                const char *a = NULL;
                gboolean    b;

                ad.present = true;
                if (g_variant_lookup(adp, "Address", "&s", &a)) snprintf(ad.addr, sizeof(ad.addr), "%s", a);
                if (g_variant_lookup(adp, "Powered", "b", &b)) ad.powered = b;
                if (g_variant_lookup(adp, "Discoverable", "b", &b)) ad.discoverable = b;
                if (g_variant_lookup(adp, "Pairable", "b", &b)) ad.pairable = b;
                g_variant_unref(adp);
            }
            const char *addr = NULL;
            const char *name = NULL;
            gboolean    paired = FALSE;
            gboolean    connected = FALSE;
            bt_dev_t    d;

            if (dev && strncmp(path, ADAPTER_PATH "/", strlen(ADAPTER_PATH) + 1) == 0) {
                g_variant_lookup(dev, "Address", "&s", &addr);
                g_variant_lookup(dev, "Name", "&s", &name);
                g_variant_lookup(dev, "Paired", "b", &paired);
                g_variant_lookup(dev, "Connected", "b", &connected);

                memset(&d, 0, sizeof(d));
                d.paired = paired;
                d.connected = connected;
                d.input = props_input(dev);
                d.audio = !d.input && props_audio(dev);
                d.phone = !d.input && !d.audio && props_phone(dev);
                if (addr && addr_ok(addr) && dev_listed(&d, addr, name, paired || connected)) {
                    snprintf(d.addr, sizeof(d.addr), "%s", addr);
                    if (paired || connected) {
                        if (n < BT_DEV_MAX) list[n++] = d;
                    } else if (nrest < BT_DEV_MAX) {
                        rest[nrest++] = d;
                    }
                }
            }
            unref(dev);
            g_variant_unref(ifaces);
        }
        g_variant_iter_free(objs);
        g_variant_unref(r);
    }
    for (int i = 0; i < nrest && n < BT_DEV_MAX; i++) {
        list[n++] = rest[i];
    }

    /* Upstream's pairing_enforce(): outside the window hidden and not
     * pairable again at once, whatever turned them on (the agent tick does
     * it too, every 30 s, with the page closed) */
    if (ad.present && (ad.discoverable || ad.pairable) && !pairing_is_open()) {
        set_prop(bus(), ADAPTER_PATH, "org.bluez.Adapter1", "Discoverable", false);
        set_prop(bus(), ADAPTER_PATH, "org.bluez.Adapter1", "Pairable", false);
        ad.discoverable = ad.pairable = false;
        plog("pairing closed: hidden, not pairable");
    }

    pthread_mutex_lock(&S.lock);
    memcpy(S.list, list, sizeof(list[0]) * n);
    S.count = n;
    S.adapter = ad;
    pthread_mutex_unlock(&S.lock);
}

/* ---- Pair / connect ------------------------------------------------------ */

/* One pair of headphones at a time (upstream's make_exclusive, for audio
 * only): the others are disconnected, not Blocked, so they still come back
 * at startup if they are on the startup list */
static void drop_other_audio(const char *keep) {
    GVariantIter *objs;
    const char   *path;
    GVariant     *ifaces;
    bool          dropped = false;
    GVariant     *r = managed_objects();

    if (!r) return;
    g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
    while (g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
        gboolean  connected = FALSE;

        if (dev) {
            g_variant_lookup(dev, "Connected", "b", &connected);
            if (connected && strcmp(path, keep) != 0 && !props_input(dev) && props_audio(dev)) {
                unref(call(path, "org.bluez.Device1", "Disconnect", NULL, NULL, CALL_TIMEOUT_MS, NULL, 0));
                plog("headphones %s disconnected: one pair at a time", path);
                dropped = true;
            }
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    }
    g_variant_iter_free(objs);
    g_variant_unref(r);
    if (dropped) sleep(2);  /* let the old link go before paging the new one */
}

/* Device1.Connect, once more after a pause: some headsets refuse the profile
 * connection while still settling after pairing */
static bool connect_dev(const char *path) {
    char      en[96];
    GVariant *props = dev_props(bus(), path);

    if (props && !props_input(props) && props_audio(props)) {
        drop_other_audio(path);
    }
    unref(props);

    for (int attempt = 1; attempt <= 2; attempt++) {
        GVariant *r = call(path, "org.bluez.Device1", "Connect", NULL, NULL, CONN_TIMEOUT_MS, en, sizeof(en));

        if (r || strstr(en, "AlreadyConnected")) {
            unref(r);
            plog("Connect: ok");
            return true;
        }
        if (attempt == 1) sleep(2);
    }
    return false;
}

/* Wait for a device to (re)appear in an inquiry */
static bool wait_known(const char *addr, bool *scanned) {
    int i;

    *scanned = discovery(true, "auto");
    plog("discovery %s, waiting up to %d s", *scanned ? "on" : "FAILED", PAIR_FIND_S);
    for (i = 0; i < PAIR_FIND_S && !device_known(addr); i++) {
        sleep(1);
    }
    if (!device_known(addr)) return false;
    plog("found after %d s", i);
    return true;
}

/* A keyboard not paired yet, at addr or under the same name: many keyboards
 * take a new address each time their pairing key is pressed, so the entry
 * pressed may be gone while the keyboard advertises under the next one.
 * The address found goes to out. (randal007) */
static bool find_keyboard(const char *name, const char *addr, char *out, size_t out_sz) {
    GVariantIter *objs;
    const char   *path;
    GVariant     *ifaces;
    bool          found = false;
    GVariant     *r = managed_objects();

    if (!r) return false;
    g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
    while (!found && g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant   *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
        const char *a = NULL;
        const char *n = NULL;
        gboolean    paired = FALSE;

        if (dev) {
            g_variant_lookup(dev, "Address", "&s", &a);
            g_variant_lookup(dev, "Name", "&s", &n);
            g_variant_lookup(dev, "Paired", "b", &paired);
            if (a && addr_ok(a) && !paired &&
                (g_ascii_strcasecmp(a, addr) == 0 || (name[0] && n && strcmp(n, name) == 0 && props_input(dev)))) {
                snprintf(out, out_sz, "%s", a);
                found = true;
            }
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    }
    g_variant_iter_free(objs);
    g_variant_unref(r);
    return found;
}

/* Wait for a keyboard to (re)appear, LE only, polled every half second as
 * its pairing window may be short */
static bool wait_keyboard(const char *name, const char *addr, char *out, size_t out_sz, bool *scanned) {
    *scanned = discovery(true, "le");
    plog("discovery (LE) %s, waiting up to %d s", *scanned ? "on" : "FAILED", PAIR_FIND_S);
    for (int i = 0; i < PAIR_FIND_S * 2; i++) {
        if (find_keyboard(name, addr, out, out_sz)) {
            plog("found %s after %d.%d s", out, i / 2, (i % 2) * 5);
            return true;
        }
        usleep(500000);
    }
    return false;
}

/* After a keyboard paired at a new address its old pairings under the same
 * name go: pressing the pairing key made the keyboard drop them. One that
 * is connected stays (a second keyboard of the same model). */
static void forget_old_keyboards(const char *name, const char *keep) {
    GVariantIter *objs;
    const char   *path;
    GVariant     *ifaces;
    GVariant     *r;

    if (!name[0] || !(r = managed_objects())) return;
    g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
    while (g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant   *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
        const char *a = NULL;
        const char *n = NULL;
        gboolean    paired = FALSE;
        gboolean    connected = FALSE;

        if (dev) {
            g_variant_lookup(dev, "Address", "&s", &a);
            g_variant_lookup(dev, "Name", "&s", &n);
            g_variant_lookup(dev, "Paired", "b", &paired);
            g_variant_lookup(dev, "Connected", "b", &connected);
            if (a && addr_ok(a) && g_ascii_strcasecmp(a, keep) != 0 && paired && !connected &&
                n && strcmp(n, name) == 0 && props_input(dev)) {
                GVariant *rr = call(ADAPTER_PATH, "org.bluez.Adapter1", "RemoveDevice",
                                    g_variant_new("(o)", path), NULL, CALL_TIMEOUT_MS, NULL, 0);

                plog("keyboard: old pairing %s forgotten: %s", a, rr ? "ok" : "failed");
                unref(rr);
            }
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    }
    g_variant_iter_free(objs);
    g_variant_unref(r);
}

/* Pair, trust, connect. The result goes to the activity line. */
static void do_pair_connect(const char *addr) {
    char      path[64];
    char      en[96];
    char      kbd_addr[18];
    char      name[64] = "";
    GVariant *r;
    bool      scanned = false;
    bool      input;
    bool      classic = false;

    dev_path(addr, path, sizeof(path));
    plog("=== pair %s", addr);
    pairing_open();

    input = device_known(addr) && path_is_input(bus(), path);
    if (input) {
        GVariant   *props = dev_props(bus(), path);
        const char *n = NULL;
        guint32     cls;

        if (props && g_variant_lookup(props, "Name", "&s", &n)) {
            snprintf(name, sizeof(name), "%s", n);
        }
        /* Only classic devices have a Class: a Bluetooth 3.0 keyboard (Rii BT11) keeps its address
         * and never shows up in the LE search below, so it pairs like headphones do */
        classic = props && g_variant_lookup(props, "Class", "u", &cls);
        unref(props);
    }
    if (input && classic) {
        plog("keyboard: classic, paired where Scan found it");
    } else if (input) {
        /* Many keyboards take a pairing only for a few seconds after their
         * pairing key, and an entry left from Scan says nothing about now:
         * the old entry goes, and Pair() is sent the moment the keyboard
         * shows up again, at its old address or a new one */
        unref(call(ADAPTER_PATH, "org.bluez.Adapter1", "RemoveDevice", g_variant_new("(o)", path),
                   NULL, CALL_TIMEOUT_MS, NULL, 0));
        set_activity("press the keyboard's pairing key");
        plog("keyboard: waiting for \"%s\" to advertise again", name);
        if (!wait_keyboard(name, addr, kbd_addr, sizeof(kbd_addr), &scanned)) {
            plog("result: keyboard not found");
            if (scanned) discovery(false, NULL);
            set_activity("keyboard not found");
            return;
        }
        addr = kbd_addr;
        dev_path(addr, path, sizeof(path));
    } else if (!device_known(addr)) {
        /* bluez forgets a scanned device ~30 s after the scan: look again */
        set_activity("searching...");
        if (!wait_known(addr, &scanned)) {
            plog("result: not found");
            if (scanned) discovery(false, NULL);
            set_activity("not found - is it in pairing mode?");
            return;
        }
        input = path_is_input(bus(), path);
    }
    /* an inquiry running alongside slows paging and pairing down */
    if (scanned) discovery(false, NULL);

    /* A keyboard often pairs with passkey entry: bluez offers it only to an
     * agent that can display, so only for this pairing; headphones stay
     * with Just Works */
    if (input) agent_set_cap(AGENT_CAP_KBD);
    set_activity("pairing...");
    r = call(path, "org.bluez.Device1", "Pair", NULL, NULL, PAIR_TIMEOUT_MS, en, sizeof(en));
    if (input) agent_set_cap(AGENT_CAP);
    if (r) {
        g_variant_unref(r);
        plog("Pair: ok");
        if (input) forget_old_keyboards(name, addr);
    } else if (strstr(en, "AlreadyExists")) {
        plog("Pair: already paired");
    } else {
        plog("result: pair failed (%s)", en[0] ? en : "no reply");
        set_activity("pairing failed");
        return;
    }

    /* Trusted: it reconnects by itself and its profiles need no agent */
    plog("Trusted: %s", set_prop(bus(), path, "org.bluez.Device1", "Trusted", true) ? "ok" : "failed");
    if (system(BTAUDIO_CTL " save") != 0) plog("btaudio-ctl save failed");

    set_activity("connecting...");
    if (!connect_dev(path)) {
        plog("result: connect failed");
        set_activity("paired, connect failed");
        return;
    }
    plog("result: connected%s", input ? " (keyboard)" : "");
    set_activity("");
}

static void do_connect(const char *addr) {
    char path[64];

    dev_path(addr, path, sizeof(path));
    if (!dev_bool(addr, "Paired", NULL)) {
        do_pair_connect(addr);
    } else {
        set_activity("connecting...");
        plog("=== connect %s", addr);
        set_prop(bus(), path, "org.bluez.Device1", "Trusted", true);
        set_activity(connect_dev(path) ? "" : "connect failed");
    }
    /* PulseAudio makes its card on the connection event: look again in a moment */
    sleep(2);
}

static void do_disconnect(const char *addr) {
    char path[64];

    dev_path(addr, path, sizeof(path));
    set_activity("disconnecting...");
    plog("=== disconnect %s", addr);
    unref(call(path, "org.bluez.Device1", "Disconnect", NULL, NULL, CALL_TIMEOUT_MS, NULL, 0));
    sleep(1);
    set_activity("");
}

/* Drop the pairing on our side: needed when the headphones lost their key
 * for this radio (reset, paired elsewhere) */
static void do_forget(const char *addr) {
    char      path[64];
    char      cmd[96];
    GVariant *r;

    dev_path(addr, path, sizeof(path));
    set_activity("removing...");
    plog("=== forget %s", addr);
    unref(call(path, "org.bluez.Device1", "Disconnect", NULL, NULL, CALL_TIMEOUT_MS, NULL, 0));
    r = call(ADAPTER_PATH, "org.bluez.Adapter1", "RemoveDevice", g_variant_new("(o)", path), NULL,
             CALL_TIMEOUT_MS, NULL, 0);
    plog("RemoveDevice: %s", r ? "ok" : "failed");
    unref(r);
    snprintf(cmd, sizeof(cmd), BTAUDIO_CTL " autoconnect %s off; " BTAUDIO_CTL " save", addr);
    if (system(cmd) != 0) plog("%s failed", cmd);
    set_activity("");
}

/* Connect, disconnect or forget waiting: a scan ends early for it */
static bool cmd_waiting(void) {
    bool w;

    pthread_mutex_lock(&S.lock);
    w = S.cmd != CMD_NONE && S.cmd != CMD_REFRESH;
    pthread_mutex_unlock(&S.lock);
    return w;
}

/* LE and classic in turns, not together (see discovery()): an LE turn is
 * never far off, so it doesn't matter whether the keyboard's pairing key
 * was pressed before Scan or during it. The list is refreshed after every
 * turn so devices appear as they are found. (randal007) */
static void do_scan(void) {
    set_activity("scanning 16 s...");
    plog("=== scan");
    for (int i = 0; i < SCAN_ROUNDS * 2 && !cmd_waiting(); i++) {
        bool le = i % 2 == 0;

        if (discovery(true, le ? "le" : "bredr")) {
            for (int q = 0; q < (le ? SCAN_LE_S : SCAN_BREDR_S) * 4 && !cmd_waiting(); q++) {
                usleep(250000);
            }
            discovery(false, NULL);
        }
        do_refresh();
    }
    set_activity("");
}

/* ---- Agent ------------------------------------------------------------ */

static const char AGENT_XML[] =
    "<node>"
    " <interface name='org.bluez.Agent1'>"
    "  <method name='Release'/>"
    "  <method name='RequestPinCode'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='s' name='pincode' direction='out'/>"
    "  </method>"
    "  <method name='DisplayPinCode'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='s' name='pincode' direction='in'/>"
    "  </method>"
    "  <method name='RequestPasskey'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='u' name='passkey' direction='out'/>"
    "  </method>"
    "  <method name='DisplayPasskey'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='u' name='passkey' direction='in'/>"
    "   <arg type='q' name='entered' direction='in'/>"
    "  </method>"
    "  <method name='RequestConfirmation'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='u' name='passkey' direction='in'/>"
    "  </method>"
    "  <method name='RequestAuthorization'>"
    "   <arg type='o' name='device' direction='in'/>"
    "  </method>"
    "  <method name='AuthorizeService'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='s' name='uuid' direction='in'/>"
    "  </method>"
    "  <method name='Cancel'/>"
    " </interface>"
    "</node>";

/* A code for a legacy (PIN) keyboard: typed on it, so not "0000" */
static void random_pin(char *out, size_t out_sz) {
    snprintf(out, out_sz, "%06u", g_random_int_range(0, 1000000));
}

/* Runs on the agent thread */
static void agent_call(GDBusConnection *c, const gchar *sender, const gchar *obj, const gchar *iface,
                       const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer user) {
    const char *dev = NULL;
    char        text[48];

    if (g_variant_n_children(params) > 0) {
        GVariant *v = g_variant_get_child_value(params, 0);

        if (g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)) {
            dev = g_variant_get_string(v, NULL);
        }
        g_variant_unref(v);     /* dev stays valid: params holds it */
    }
    plog("agent: %s %s", method, dev ? dev : "");

    if (strcmp(method, "Release") == 0) {
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }
    if (strcmp(method, "Cancel") == 0) {
        pthread_mutex_lock(&S.lock);
        if (strncmp(S.activity, "type ", 5) == 0) S.activity[0] = '\0';
        pthread_mutex_unlock(&S.lock);
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }
    if (strcmp(method, "AuthorizeService") == 0 ? !pairing_is_open() && !path_is_paired(A.conn, dev)
                                                : !pairing_is_open()) {
        plog("agent: refused %s, pairing is closed", method);
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "Pairing closed");
        return;
    }

    if (strcmp(method, "RequestPinCode") == 0) {
        if (path_is_input(A.conn, dev)) {
            char pin[8];

            random_pin(pin, sizeof(pin));
            snprintf(text, sizeof(text), "type %s and Enter on the keyboard", pin);
            set_activity(text);
            plog("agent: PIN %s for the keyboard", pin);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", pin));
        } else {
            g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", AGENT_PIN));
        }
    } else if (strcmp(method, "DisplayPinCode") == 0) {
        const char *pin = NULL;

        g_variant_get(params, "(&o&s)", NULL, &pin);
        snprintf(text, sizeof(text), "type %.8s and Enter on the keyboard", pin ? pin : "?");
        set_activity(text);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (strcmp(method, "DisplayPasskey") == 0) {
        guint32 passkey = 0;
        guint16 entered = 0;

        g_variant_get(params, "(&ouq)", NULL, &passkey, &entered);
        snprintf(text, sizeof(text), "type %06u and Enter on the keyboard", (unsigned)passkey);
        set_activity(text);
        plog("agent: passkey %06u (%u typed)", (unsigned)passkey, (unsigned)entered);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (strcmp(method, "RequestPasskey") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0u));
    } else {
        /* RequestConfirmation, RequestAuthorization, AuthorizeService */
        g_dbus_method_invocation_return_value(inv, NULL);
    }
}

static const GDBusInterfaceVTable agent_vtable = { .method_call = agent_call };

static bool agent_mgr(const char *method, GVariant *params) {
    GVariant *r = call_on(A.conn, "/org/bluez", "org.bluez.AgentManager1", method, params, NULL,
                          CALL_TIMEOUT_MS, NULL, 0);

    unref(r);
    return r != NULL;
}

static void agent_register(void) {
    char cap[20];

    pthread_mutex_lock(&S.lock);
    snprintf(cap, sizeof(cap), "%s", S.agent_cap[0] ? S.agent_cap : AGENT_CAP);
    pthread_mutex_unlock(&S.lock);

    if (agent_mgr("RegisterAgent", g_variant_new("(os)", AGENT_PATH, cap)) &&
        agent_mgr("RequestDefaultAgent", g_variant_new("(o)", AGENT_PATH))) {
        plog("agent: registered (%s)", cap);
    }
}

/* Worker thread: register again with another IO capability, bluez reads it
 * when a pairing starts */
static void agent_set_cap(const char *cap) {
    pthread_mutex_lock(&S.lock);
    if (strcmp(S.agent_cap[0] ? S.agent_cap : AGENT_CAP, cap) == 0) {
        pthread_mutex_unlock(&S.lock);
        return;
    }
    snprintf(S.agent_cap, sizeof(S.agent_cap), "%s", cap);
    pthread_mutex_unlock(&S.lock);

    agent_mgr("UnregisterAgent", g_variant_new("(o)", AGENT_PATH));
    agent_register();
}

/* Every 30 s: default agent again (another agent may have taken the place),
 * and hidden / not pairable outside the window, also after a bluetoothd
 * restart or the adapter coming back */
static gboolean agent_tick(gpointer user) {
    if (!agent_mgr("RequestDefaultAgent", g_variant_new("(o)", AGENT_PATH))) {
        agent_register();
    }
    if (!pairing_is_open() &&
        (get_prop(A.conn, ADAPTER_PATH, "org.bluez.Adapter1", "Discoverable", NULL) ||
         get_prop(A.conn, ADAPTER_PATH, "org.bluez.Adapter1", "Pairable", NULL))) {
        set_prop(A.conn, ADAPTER_PATH, "org.bluez.Adapter1", "Discoverable", false);
        set_prop(A.conn, ADAPTER_PATH, "org.bluez.Adapter1", "Pairable", false);
        plog("pairing closed: hidden, not pairable");
    }
    return G_SOURCE_CONTINUE;
}

static void bluez_appeared(GDBusConnection *c, const gchar *name, const gchar *owner, gpointer user) {
    agent_register();
}

static void *agent_thread(void *arg) {
    GError        *err = NULL;
    GDBusNodeInfo *info;
    GSource       *tick;

    A.ctx = g_main_context_new();
    /* agent calls and the name watch are dispatched in this thread's context */
    g_main_context_push_thread_default(A.ctx);

    A.conn = bus_new();
    info = g_dbus_node_info_new_for_xml(AGENT_XML, &err);
    if (!A.conn || !info ||
        !g_dbus_connection_register_object(A.conn, AGENT_PATH, info->interfaces[0], &agent_vtable, NULL, NULL, &err)) {
        plog("agent: not started: %s", err ? err->message : "no D-Bus");
        g_clear_error(&err);
        return NULL;
    }
    /* (re)register with bluez every time bluetoothd appears */
    g_bus_watch_name_on_connection(A.conn, BLUEZ, G_BUS_NAME_WATCHER_FLAGS_NONE, bluez_appeared, NULL, NULL, NULL);
    tick = g_timeout_source_new_seconds(AGENT_TICK_S);
    g_source_set_callback(tick, agent_tick, NULL, NULL);
    g_source_attach(tick, A.ctx);

    for (;;) {
        g_main_context_iteration(A.ctx, TRUE);
    }
    return NULL;
}

/* ---- Worker ------------------------------------------------------------ */

static void *worker(void *arg) {
    for (;;) {
        cmd_t cmd;
        char  addr[18];

        pthread_mutex_lock(&S.lock);
        while (S.cmd == CMD_NONE) {
            pthread_cond_wait(&S.cond, &S.lock);
        }
        cmd = S.cmd;
        S.cmd = CMD_NONE;
        S.busy = cmd != CMD_REFRESH && cmd != CMD_SCAN;
        S.scanning = cmd == CMD_SCAN;
        snprintf(addr, sizeof(addr), "%s", S.cmd_addr);
        pthread_mutex_unlock(&S.lock);

        switch (cmd) {
            case CMD_CONNECT:    do_connect(addr);    break;
            case CMD_DISCONNECT: do_disconnect(addr); break;
            case CMD_SCAN:       do_scan();           break;
            case CMD_FORGET:     do_forget(addr);     break;
            default:                                  break;
        }
        do_refresh();

        pthread_mutex_lock(&S.lock);
        S.busy = false;
        S.scanning = false;
        pthread_mutex_unlock(&S.lock);
    }
    return NULL;
}

static void post(cmd_t cmd, const char *addr) {
    if (addr && !addr_ok(addr)) return;
    pthread_mutex_lock(&S.lock);
    /* one slot: a refresh never overwrites a command not picked up yet, nor
     * piles up behind a scan */
    if (cmd != CMD_REFRESH || (S.cmd == CMD_NONE && !S.scanning)) {
        S.cmd = cmd;
        if (addr) snprintf(S.cmd_addr, sizeof(S.cmd_addr), "%s", addr);
        if (cmd != CMD_REFRESH) S.activity[0] = '\0';
        pthread_cond_signal(&S.cond);
    }
    pthread_mutex_unlock(&S.lock);
}

/* ------------------------------------------------------------------ */

void bt_dev_init(void) {
    pthread_t agent;

    pthread_create(&S.thread, NULL, worker, NULL);
    pthread_detach(S.thread);
    pthread_create(&agent, NULL, agent_thread, NULL);
    pthread_detach(agent);
}

void bt_dev_refresh(void) {
    post(CMD_REFRESH, NULL);
}

int bt_dev_count(void) {
    int n;

    pthread_mutex_lock(&S.lock);
    n = S.count;
    pthread_mutex_unlock(&S.lock);
    return n;
}

bool bt_dev_get(int idx, bt_dev_t *out) {
    bool ok;

    pthread_mutex_lock(&S.lock);
    ok = idx >= 0 && idx < S.count;
    if (ok) *out = S.list[idx];
    pthread_mutex_unlock(&S.lock);
    return ok;
}

void bt_dev_connect(const char *addr) {
    post(CMD_CONNECT, addr);
}

void bt_dev_disconnect(const char *addr) {
    post(CMD_DISCONNECT, addr);
}

void bt_dev_forget(const char *addr) {
    post(CMD_FORGET, addr);
}

void bt_dev_scan(void) {
    post(CMD_SCAN, NULL);
}

bool bt_dev_scanning(void) {
    bool s;

    pthread_mutex_lock(&S.lock);
    s = S.scanning || S.cmd == CMD_SCAN;
    pthread_mutex_unlock(&S.lock);
    return s;
}

bool bt_dev_busy(void) {
    bool b;

    pthread_mutex_lock(&S.lock);
    b = S.busy || (S.cmd != CMD_NONE && S.cmd != CMD_REFRESH && S.cmd != CMD_SCAN);
    pthread_mutex_unlock(&S.lock);
    return b;
}

void bt_dev_activity(char *out, int size) {
    pthread_mutex_lock(&S.lock);
    snprintf(out, size, "%s", S.activity);
    pthread_mutex_unlock(&S.lock);
}

void bt_dev_adapter(bt_adapter_t *out) {
    pthread_mutex_lock(&S.lock);
    *out = S.adapter;
    pthread_mutex_unlock(&S.lock);
}
