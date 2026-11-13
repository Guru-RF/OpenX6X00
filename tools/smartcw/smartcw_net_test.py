#!/usr/bin/env python3
"""SmartCW's USB network on the radio, as files: the udev rule that names it and the NetworkManager profile that takes
it. See the Makefile:

    python3 -I -B tools/smartcw/smartcw_net_test.py [-v]

1. The udev rule (rootfs/etc/udev/rules.d/72-smartcw.rules) read as udev reads a rule: one rule, every key known, net
   interfaces added, SmartCW's USB IDs (those src/paddle_keyer.c knows it by), the name smartcw<n> from the kernel's
   usb0 or eth1, short enough for an interface.
2. The profile (rootfs/usr/lib/NetworkManager/system-connections/smartcw.nmconnection) read as a GLib key file: its
   keys known to NetworkManager, an ethernet profile matched by the interface name alone, DHCP, never the default
   route nor the DNS, its subnet's route after WiFi's and Ethernet's, IPv6 off, and a priority above the GUI's
   "Ethernet" profile (src/wifi.cpp, which sets none: 0).
3. The name agreed on by the rule, the profile, the GUI's Ethernet code and the web server's proxy.
4. CMakeLists.txt's own install rule for rootfs/, run by cmake into a folder of the test's: the profile 0600 (else
   NetworkManager leaves it out), the rest with the modes they have in the tree.
"""
import configparser
import fnmatch
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, "..", ".."))
RULE = os.path.join(TOP, "rootfs", "etc", "udev", "rules.d", "72-smartcw.rules")
PROFILE_PATH = "usr/lib/NetworkManager/system-connections/smartcw.nmconnection"
PROFILE = os.path.join(TOP, "rootfs", PROFILE_PATH)
TETHER = os.path.join(TOP, "buildroot", "br2_external", "board", "X6100", "linux", "rootfs-overlay", "etc",
                      "NetworkManager", "system-connections", "usb-tether.nmconnection")
sys.path[:0] = [os.environ.get("WEBSERVER") or os.path.join(TOP, "webserver", "src")]
sys.dont_write_bytecode = True
from x6100_webserver import smartcw  # noqa: E402

# udev's keys a rule may have (eudev 3.2's), and the operators
UDEV_KEYS = {"ACTION", "DEVPATH", "KERNEL", "NAME", "SYMLINK", "SUBSYSTEM", "DRIVER", "ATTR", "KERNELS", "SUBSYSTEMS",
             "DRIVERS", "ATTRS", "TAGS", "ENV", "TAG", "TEST", "PROGRAM", "RESULT", "OWNER", "GROUP", "MODE", "LABEL",
             "GOTO", "IMPORT", "RUN", "OPTIONS", "SECLABEL"}
UDEV_TOKEN = re.compile(r'\s*([A-Z]+)(\{[^}]*\})?\s*(==|!=|\+=|-=|:=|=)\s*"([^"]*)"\s*(,|$)')
# The profile's keys NetworkManager knows, by section (those this profile may use)
NM_KEYS = {"connection": {"id", "uuid", "type", "autoconnect", "autoconnect-priority", "autoconnect-retries"},
           "match": {"interface-name", "driver"},
           "ethernet": set(),
           "ipv4": {"method", "never-default", "ignore-auto-dns", "ignore-auto-routes", "route-metric",
                    "dhcp-timeout", "may-fail"},
           "ipv6": {"method"}}
WIFI_METRIC, ETHERNET_METRIC = 600, 100    # NetworkManager's own for WiFi and wired


def read(path):
    with open(path) as f:
        return f.read()


def udev_rules(path):
    """The rules of a rules file: a list of (key, attribute, operator, value) lists"""
    rules = []
    for n, line in enumerate(read(path).splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        pos, rule = 0, []
        while pos < len(line):
            m = UDEV_TOKEN.match(line, pos)
            if not m:
                raise ValueError(f"{path}:{n}: not a udev rule from column {pos + 1}")
            key, attr, op, value, _ = m.groups()
            if key not in UDEV_KEYS:
                raise ValueError(f"{path}:{n}: no such udev key {key}")
            rule.append((key, (attr or "")[1:-1], op, value))
            pos = m.end()
        rules.append(rule)
    return rules


def udev_match(pattern, value):
    """udev's match of a value: shell patterns, | between alternatives"""
    return any(fnmatch.fnmatchcase(value, p) for p in pattern.split("|"))


def udev_name(value, kernel_name):
    """NAME's value for an interface the kernel named kernel_name: %n is its number, %k its name"""
    number = re.search(r"\d*$", kernel_name).group()
    return value.replace("%n", number).replace("%k", kernel_name)


def keyfile(path):
    """A NetworkManager profile as a GLib key file reads it: sections, keys as written, # comments"""
    c = configparser.ConfigParser(interpolation=None, delimiters=("=",), comment_prefixes=("#",),
                                  inline_comment_prefixes=None, strict=True, empty_lines_in_values=False)
    c.optionxform = str
    c.read_string(read(path), source=path)
    return c


def glib_list(value):
    return [v for v in value.split(";") if v]


class UdevRule(unittest.TestCase):
    def setUp(self):
        rules = udev_rules(RULE)
        self.assertEqual(len(rules), 1, "one rule")
        self.rule = rules[0]
        self.matches = {(k, a): v for k, a, op, v in self.rule if op == "=="}
        self.assigns = {(k, a): v for k, a, op, v in self.rule if op == "="}

    def test_net_interfaces_added(self):
        self.assertEqual(self.matches.get(("SUBSYSTEM", "")), "net")
        self.assertEqual(self.matches.get(("ACTION", "")), "add")

    def test_smartcw_ids(self):
        """SmartCW's IDs as src/paddle_keyer.c has them: vendor 1209, product ff42 or ff43"""
        src = read(os.path.join(TOP, "src", "paddle_keyer.c"))
        m = re.search(r"vendor == 0x([0-9a-f]{4}) && \(product == 0x([0-9a-f]{4}) \|\| product == 0x([0-9a-f]{4})\)",
                      src)
        self.assertTrue(m, "paddle_smartcw() not found in src/paddle_keyer.c")
        vendor, *products = m.groups()
        self.assertTrue(udev_match(self.matches[("ATTRS", "idVendor")], vendor))
        for p in products:
            self.assertTrue(udev_match(self.matches[("ATTRS", "idProduct")], p), p)
        for other in ("ff41", "ff44", "ff4", "0000"):
            self.assertFalse(udev_match(self.matches[("ATTRS", "idProduct")], other), other)
        self.assertFalse(udev_match(self.matches[("ATTRS", "idVendor")], "1208"))

    def test_name(self):
        self.assertEqual(set(self.assigns), {("NAME", "")}, "the rule only names the interface")
        for kernel, want in (("usb0", "smartcw0"), ("eth1", "smartcw1"), ("usb12", "smartcw12")):
            name = udev_name(self.assigns[("NAME", "")], kernel)
            self.assertEqual(name, want)
            self.assertLessEqual(len(name), 15, "IFNAMSIZ")


class Profile(unittest.TestCase):
    def setUp(self):
        self.c = keyfile(PROFILE)

    def test_known_keys(self):
        self.assertEqual(set(self.c.sections()), set(NM_KEYS))
        for section in self.c.sections():
            self.assertLessEqual(set(self.c[section]), NM_KEYS[section], section)

    def test_connection(self):
        con = self.c["connection"]
        self.assertEqual(con["type"], "ethernet")
        self.assertEqual(con["id"], "SmartCW")
        self.assertEqual(str(uuid.UUID(con["uuid"])), con["uuid"])
        self.assertNotEqual(con["uuid"], keyfile(TETHER)["connection"]["uuid"])
        self.assertNotIn("interface-name", con, "no wildcards there: [match] has the name")

    def test_priority_above_the_guis(self):
        """The GUI's "Ethernet" profile (eth_connection_new) has no match and no priority: 0, NetworkManager's
        default. SmartCW's takes the interface before it"""
        gui = read(os.path.join(TOP, "src", "wifi.cpp"))
        new = gui[gui.index("static NMConnection *eth_connection_new"):]
        new = new[:new.index("\n}\n")]
        self.assertNotIn("AUTOCONNECT_PRIORITY", new)
        self.assertNotIn("NMSettingMatch", new)
        self.assertGreater(int(self.c["connection"]["autoconnect-priority"]), 0)

    def test_match(self):
        names = glib_list(self.c["match"]["interface-name"])
        self.assertEqual(names, ["smartcw*"])
        self.assertNotIn("driver", self.c["match"], "cdc_ncm is many USB Ethernet adapters' driver too")
        for iface, want in (("smartcw0", True), ("smartcw1", True), ("eth0", False), ("usb0", False),
                            ("enx001122334455", False), ("wlan0", False)):
            self.assertEqual(any(fnmatch.fnmatchcase(iface, n) for n in names), want, iface)

    def test_ipv4(self):
        ip = self.c["ipv4"]
        self.assertEqual(ip["method"], "auto")
        self.assertEqual(ip["never-default"], "true")
        self.assertEqual(ip["ignore-auto-dns"], "true")
        self.assertGreater(int(ip["route-metric"]), max(WIFI_METRIC, ETHERNET_METRIC))

    def test_ipv6(self):
        self.assertEqual(self.c["ipv6"]["method"], "disabled")


class Names(unittest.TestCase):
    def test_one_name(self):
        rule = [v for k, a, op, v in udev_rules(RULE)[0] if k == "NAME"][0]
        prefix = rule.split("%")[0]
        self.assertEqual(prefix, "smartcw")
        self.assertEqual(glib_list(keyfile(PROFILE)["match"]["interface-name"]), [prefix + "*"])
        self.assertEqual(smartcw.IFACE_PREFIX, prefix)
        self.assertIn(f'g_str_has_prefix(nm_device_get_iface(d), "{prefix}")', read(os.path.join(TOP, "src",
                                                                                               "wifi.cpp")))


class Install(unittest.TestCase):
    def test_modes(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("no cmake on this PC")
        top = read(os.path.join(TOP, "CMakeLists.txt"))
        start = top.index("install(DIRECTORY rootfs/")
        rule = top[start:top.index(")", start) + 1]
        with tempfile.TemporaryDirectory() as d:
            src, build, dest = (os.path.join(d, n) for n in ("src", "build", "dest"))
            os.mkdir(src)
            with open(os.path.join(src, "CMakeLists.txt"), "w") as f:
                f.write("cmake_minimum_required(VERSION 3.10)\nproject(rootfs_modes NONE)\n" +
                        rule.replace("rootfs/", os.path.join(TOP, "rootfs") + "/", 1) + "\n")
            for args in ([cmake, "-S", src, "-B", build], [cmake, "--install", build]):
                r = subprocess.run(args, capture_output=True, text=True, env={**os.environ, "DESTDIR": dest})
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

            def mode(path):
                return stat.S_IMODE(os.stat(os.path.join(dest, path)).st_mode)

            self.assertEqual(mode(PROFILE_PATH), 0o600)
            self.assertEqual(mode("etc/udev/rules.d/72-smartcw.rules"), 0o644)
            self.assertEqual(mode("usr/sbin/x6100-update"), 0o755)


if __name__ == "__main__":
    unittest.main()
