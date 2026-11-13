#!/usr/bin/env python3
"""SmartCW's NetworkManager profile with a NetworkManager of this PC. See the Makefile:

    unshare -rmn python3 -I -B tools/smartcw/smartcw_nm_test.py [-v]

Run in namespaces of its own, as tools/wifi_restore: no network but loopback and the test's own links,
NetworkManager's folders (/run, /var/lib/NetworkManager, /etc/NetworkManager and
/usr/lib/NetworkManager/system-connections) on tmpfs, its D-Bus a bus of its own at the system bus's usual place, so
nothing of this PC's network is touched. Without udev (/sys read-only), so the links have the names the test gives
them: smartcw0 as udev names SmartCW's (72-smartcw.rules), eth9 as a USB Ethernet adapter's.

1. The profile as an update brings it, after NetworkManager's start: not known until the web server's start tells
   NetworkManager to read its profiles (smartcw.follow_profile, apps.NM_RELOAD by dbus-send, which the radios
   have), then known with its settings. A file older than NetworkManager's start is left alone. Installed 0644 (as
   git has it) NetworkManager leaves it out: CMakeLists.txt installs it 0600. Put in place 0755 (as the updaters up
   to v0.3.12 left every file, staged on the FAT DATA partition), also older than NetworkManager's start, the web
   server makes it 0600 and tells NetworkManager.
2. With the GUI's own "Ethernet" profile saved (src/wifi.cpp eth_connection_new: no match, no priority):
   NetworkManager takes SmartCW's profile on smartcw0 and the GUI's on eth9, never SmartCW's.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path[:0] = [os.environ.get("WEBSERVER") or os.path.join(TOP, "webserver", "src")]
sys.dont_write_bytecode = True
from x6100_webserver import smartcw  # noqa: E402

PROFILE = os.path.join(TOP, "rootfs", "usr", "lib", "NetworkManager", "system-connections", "smartcw.nmconnection")
HOST_PATH = os.environ.get("PATH", "") + ":/usr/sbin:/sbin"
BUS = "/run/dbus/system_bus_socket"
# As tools/wifi_restore, the far ends of the test's links left alone (else the GUI's profile goes to one of them),
# and its log on stderr (KEEP=1 keeps it)
NM_CONF = """[main]
plugins=keyfile
dns=none
auth-polkit=false
no-auto-default=*
[keyfile]
unmanaged-devices=interface-name:scpeer0;interface-name:ethpeer9
[connectivity]
enabled=false
[logging]
backend=debug
level=INFO
"""
BUS_CONF = f"""<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>system</type><listen>unix:path={BUS}</listen><auth>EXTERNAL</auth>
<policy context="default"><allow user="*"/><allow own="*"/><allow send_type="method_call"/>
<allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>
"""
# The GUI's "Ethernet" profile as eth_connection_new saves it, DHCP
GUI_ETHERNET = """[connection]
id=Ethernet
uuid=5d0c8b6a-1f2e-4a3b-9c4d-5e6f7a8b9c0d
type=ethernet

[ethernet]

[ipv4]
method=auto

[ipv6]
method=disabled
"""
NM_DIR = "/usr/lib/NetworkManager/system-connections"
procs = []
work = None


def tool(name):
    return shutil.which(name, path=HOST_PATH)


def nm(*args):
    env = {k: v for k, v in os.environ.items() if k != "DBUS_SYSTEM_BUS_ADDRESS"}
    return subprocess.run([tool("nmcli"), *args], capture_output=True, text=True, timeout=20,
                          env={**env, "PATH": HOST_PATH})


def known():
    return [line.rsplit(":", 1)[0] for line in nm("-t", "-f", "NAME,UUID", "connection", "show").stdout.splitlines()]


def until(get, want, secs=15):
    end = time.monotonic() + secs
    while (got := get()) != want and time.monotonic() < end:
        time.sleep(0.2)
    return got


def setUpModule():
    global work
    missing = [t for t in ("nmcli", "NetworkManager", "dbus-daemon", "dbus-send", "ip", "mount") if not tool(t)]
    if missing:
        raise unittest.SkipTest(f"not on this PC: {', '.join(missing)}")
    if os.getuid() != 0:
        sys.exit("run it in namespaces of its own: make (unshare -rmn)")
    with open("/proc/net/dev") as f:
        links = [line.split(":")[0].strip() for line in f.readlines()[2:]]
    if links != ["lo"]:
        sys.exit(f"not a network namespace of its own ({', '.join(links)}): make runs it with unshare -rmn")
    if not os.path.isdir(NM_DIR):
        raise unittest.SkipTest(f"no {NM_DIR} on this PC to put a tmpfs on")
    for args in (["-t", "sysfs", "-o", "ro", "sysfs", "/sys"],    # read-only: NetworkManager then does without udev
                 ["-t", "tmpfs", "tmpfs", "/run"],
                 ["-t", "tmpfs", "tmpfs", "/var/lib/NetworkManager"],
                 ["-t", "tmpfs", "tmpfs", "/etc/NetworkManager"],
                 ["-t", "tmpfs", "tmpfs", NM_DIR]):
        subprocess.run([tool("mount"), *args], check=True)
    work = tempfile.mkdtemp(prefix="x6100-smartcw-nm.")
    for d in ("/run/dbus", "/run/NetworkManager", "/etc/NetworkManager/system-connections"):
        os.makedirs(d, exist_ok=True)
    with open("/etc/NetworkManager/NetworkManager.conf", "w") as f:
        f.write(NM_CONF)
    gui = "/etc/NetworkManager/system-connections/Ethernet.nmconnection"
    with open(gui, "w") as f:
        f.write(GUI_ETHERNET)
    os.chmod(gui, 0o600)
    with open(os.path.join(work, "bus.conf"), "w") as f:
        f.write(BUS_CONF)
    log = open(os.path.join(work, "nm.log"), "w")
    procs.append(subprocess.Popen([tool("dbus-daemon"), "--config-file=" + os.path.join(work, "bus.conf"), "--nofork",
                                   "--nopidfile"]))
    for _ in range(50):
        if os.path.exists(BUS):
            break
        time.sleep(0.1)
    procs.append(subprocess.Popen([tool("NetworkManager"), "--no-daemon", "--debug",
                                   "--config=/etc/NetworkManager/NetworkManager.conf"], stdout=log, stderr=log))
    log.close()
    for _ in range(100):
        if nm("-t", "general", "status").returncode == 0:
            break
        time.sleep(0.2)
    else:
        sys.exit("NetworkManager did not come up: " + open(os.path.join(work, "nm.log")).read()[-2000:])
    time.sleep(1.1)   # the profile placed after it is a second newer, as after an update (mtime's whole seconds)


def tearDownModule():
    for p in reversed(procs):
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
    if work and not os.environ.get("KEEP"):
        shutil.rmtree(work, ignore_errors=True)
    elif work:
        print("NetworkManager's log:", os.path.join(work, "nm.log"))


RELOAD = ["dbus-send", "--system", "--print-reply", "--dest=org.freedesktop.NetworkManager",
          "/org/freedesktop/NetworkManager/Settings", "org.freedesktop.NetworkManager.Settings.ReloadConnections"]


class AfterAnUpdate(unittest.TestCase):
    """1, in order: the profile arrives after NetworkManager's start"""

    def test_1_read_when_told(self):
        dest = os.path.join(NM_DIR, "smartcw.nmconnection")
        self.assertFalse(smartcw.follow_profile(RELOAD, dest), "no file: nothing asked")
        shutil.copyfile(PROFILE, dest)
        os.chmod(dest, 0o644)                         # as git has it
        subprocess.run(RELOAD, check=True, capture_output=True)
        self.assertNotIn("SmartCW", known(), "0644: NetworkManager takes it after all")
        os.chmod(dest, 0o600)                         # as CMakeLists.txt installs it
        self.assertNotIn("SmartCW", known(), "known before NetworkManager was told")
        os.chmod(dest, 0o755)                         # as the updaters up to v0.3.12 put every file in place
        started = smartcw.process_started("NetworkManager")
        os.utime(dest, (started - 60, started - 60))
        self.assertTrue(smartcw.follow_profile(RELOAD, dest), "0755, older than NetworkManager: not told")
        self.assertEqual(stat.S_IMODE(os.stat(dest).st_mode), 0o600, "0755: not made 0600")
        self.assertTrue(until(lambda: "SmartCW" in known(), True), "made 0600 and told, and not known")
        self.assertFalse(smartcw.follow_profile(RELOAD, dest), "0600, older than NetworkManager: asked anyway")
        os.utime(dest)
        self.assertTrue(smartcw.follow_profile(RELOAD, dest), "newer than NetworkManager: not told")
        got = nm("-g", "connection.autoconnect-priority,match.interface-name,ipv4.method,ipv4.never-default,"
                 "ipv4.ignore-auto-dns,ipv4.route-metric,ipv6.method", "connection", "show", "SmartCW").stdout
        self.assertEqual(got.split("\n")[:7], ["50", "smartcw*", "auto", "yes", "yes", "1000", "disabled"])

    def test_2_which_profile_where(self):
        """2: SmartCW's on smartcw0, the GUI's on the adapter"""
        if "SmartCW" not in known():
            self.skipTest("the profile is not known (case 1 failed)")
        ip = tool("ip")
        for name, peer in (("smartcw0", "scpeer0"), ("eth9", "ethpeer9")):
            r = subprocess.run([ip, "link", "add", name, "type", "veth", "peer", "name", peer], capture_output=True,
                               text=True)
            if r.returncode != 0:   # veth is a module, which a namespace of one's own can't load
                self.skipTest(f"no veth link here: {r.stderr.strip()}")
            subprocess.run([ip, "link", "set", peer, "up"], check=True)

        def on(dev):
            return nm("-g", "GENERAL.CONNECTION", "device", "show", dev).stdout.strip()

        self.assertEqual(until(lambda: on("smartcw0"), "SmartCW"), "SmartCW")
        self.assertEqual(until(lambda: on("eth9"), "Ethernet"), "Ethernet")


if __name__ == "__main__":
    unittest.main()
