#!/usr/bin/env python3
"""The Backup page's WiFi restore on a PC: the web server's own code (webserver apps.py) through bottle's WSGI app,
a backup uploaded and its WiFi part restored, against a NetworkManager of this PC. See the Makefile:

    make          (unshare -rmn python3 -I -B wifi_restore_test.py [-v])

Run in namespaces of its own (unshare -rmn): no network but loopback, NetworkManager's folders (/run,
/var/lib/NetworkManager, /etc/NetworkManager) on tmpfs, its D-Bus a bus of its own at the system bus's usual
place, so nothing of this PC's network is touched. The web server's commands find only the programs that every
radio image in TARGETS has (links to this PC's in a folder of their own, PATH as on the radio): no nmcli there.

1. A network the radio did not have: NetworkManager has it a moment after the restore answers, without a restart.
2. A network it had, with another SSID in the backup: NetworkManager has the SSID of the backup.
3. NetworkManager not reachable: the restore still puts the file in place and says so, as before; the
   network is known at NetworkManager's next start.
4. The profile of the link the browser is on (here a dummy link with a static address, as an Ethernet one),
   replaced by another radio's (another UUID, another address): the answer is out before NetworkManager leaves
   that link, then the restored profile is the active one.
5. A network the radio has under another file name (the GUI names its files after the SSID, another system after
   whatever it called the network), twice in the backup: one profile of it after, the backup's latest used,
   written over the radio's file, and the radio's own kept as the safety copy.

The factory reset's WiFi choice (rootfs/usr/sbin/x6100-wifi-forget, #257), with the tree's
S04restore_backup_config making the copy on DATA (/mnt, /etc/ssh and /root on tmpfs here too):

6. The WiFi profiles go (an older file's 802-11-wireless too, a name with spaces and brackets), the Ethernet and
   USB tethering ones stay; NetworkManager drops the WiFi ones at once, and the copy on DATA is made again
   without them, the others and the SSH key still in it.
7. No WiFi profile: nothing deleted, the copy left as it was.
8. NetworkManager not reachable: the files and the copy are done all the same; it drops them at its next start.
9. The copy not made again (S04restore_backup_config failing): the copy that still names a deleted network goes.

What NetworkManager has is read with this PC's nmcli (the test's own look, not the web server's).
WEBSERVER=<a webserver/src> tests another web server (make v038: v0.3.8's, which has to fail 1, 2, 4 and 5).
"""
import glob
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import types
import unittest
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + \
    [os.environ.get("WEBSERVER") or os.path.join(TOP, "webserver", "src")]
sys.dont_write_bytecode = True
try:
    import cgi  # noqa: F401
except ImportError:     # gone in Python 3.13; bottle 0.12, the radio's, imports it, for forms only
    cgi = types.ModuleType("cgi")

    class FieldStorage:
        def __init__(self, **kw):
            raise NotImplementedError("no cgi module on this Python")
    cgi.FieldStorage = FieldStorage
    sys.modules["cgi"] = cgi
try:
    import bottle  # noqa: F401
except ImportError:
    sys.exit("bottle not found: give BOTTLE=<a folder with bottle.py>, to make or in the environment")
from x6100_webserver import apps, settings  # noqa: E402

HOST_PATH = os.environ.get("PATH", "") + ":/usr/sbin:/sbin"
TARGETS = os.environ.get("TARGETS", "").split() or \
    sorted(glob.glob(os.path.join(TOP, "..", "Aether*Buildroot", "build", "target")))
BUS = "/run/dbus/system_bus_socket"
NM_CONF = """[main]
plugins=keyfile
dns=none
auth-polkit=false
no-auto-default=*
[connectivity]
enabled=false
"""
BUS_CONF = f"""<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>system</type><listen>unix:path={BUS}</listen><auth>EXTERNAL</auth>
<policy context="default"><allow user="*"/><allow own="*"/><allow send_type="method_call"/>
<allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>
"""
UUID_HOME = "6a1d3c1e-0b7e-4b55-9d0e-2f3a4b5c6d01"
UUID_CLUB = "6a1d3c1e-0b7e-4b55-9d0e-2f3a4b5c6d02"
UUID_ETH = "6a1d3c1e-0b7e-4b55-9d0e-2f3a4b5c6d03"
UUID_ETH_OTHER = "7b2e4d2f-1c8f-4c66-8e1f-3a4b5c6d7e04"     # the same profile on another radio: its own UUID
procs = []
work = None


def tool(name):
    path = shutil.which(name, path=HOST_PATH)
    if not path:
        sys.exit(f"{name} not found on this PC")
    return path


NMCLI = tool("nmcli")


def radio_bin(root):
    """A folder of links to the programs of this PC that every radio image in TARGETS has too"""
    names = None
    for t in TARGETS:
        have = set()
        for d in ("bin", "sbin", "usr/bin", "usr/sbin"):
            try:
                have.update(os.listdir(os.path.join(t, d)))
            except OSError:
                pass
        names = have if names is None else names & have
    folder = os.path.join(root, "radio-bin")
    os.mkdir(folder)
    for n in sorted(names or ()):
        path = shutil.which(n, path=HOST_PATH)
        if path:
            os.symlink(path, os.path.join(folder, n))
    return folder


def nm(*args):
    """This PC's nmcli on the test's NetworkManager (the bus at BUS, whatever the web server is given)"""
    env = {k: v for k, v in os.environ.items() if k != "DBUS_SYSTEM_BUS_ADDRESS"}
    return subprocess.run([NMCLI, *args], capture_output=True, text=True, timeout=20, env={**env, "PATH": HOST_PATH})


def known():
    """The WiFi networks NetworkManager has now: name -> SSID"""
    out = {}
    for line in nm("-t", "-f", "NAME,UUID,TYPE", "connection", "show").stdout.splitlines():
        name, uid, kind = line.rsplit(":", 2)
        if kind == "802-11-wireless":
            out[name] = nm("-g", "802-11-wireless.ssid", "connection", "show", uid).stdout.strip()
    return out


def link():
    """The profile active on the test's wired link and the link's address: (UUID, address), else None"""
    for line in nm("-t", "-f", "UUID,DEVICE,STATE", "connection", "show", "--active").stdout.splitlines():
        uid, dev, state = line.rsplit(":", 2)
        if dev == "dum0" and state == "activated":
            return uid, nm("-g", "IP4.ADDRESS", "device", "show", "dum0").stdout.strip()
    return None


def until(get, want, secs=20):
    """get() once it gives want, or what it gives after secs"""
    end = time.monotonic() + secs
    while (got := get()) != want and time.monotonic() < end:
        time.sleep(0.2)
    return got


def reloaded():
    """The web server's reload of NetworkManager, made a moment after the restore's answer, has run"""
    for t in threading.enumerate():
        if t is not threading.main_thread():
            t.join(60)


def setUpModule():
    global work
    if os.getuid() != 0:
        sys.exit("run it in namespaces of its own: make (unshare -rmn)")
    with open("/proc/net/dev") as f:
        links = [line.split(":")[0].strip() for line in f.readlines()[2:]]
    if links != ["lo"]:
        sys.exit(f"not a network namespace of its own ({', '.join(links)}): make runs it with unshare -rmn")
    if not TARGETS:
        sys.exit("no radio image found: give TARGETS=<buildroot target folders>")
    for args in (["-t", "sysfs", "-o", "ro", "sysfs", "/sys"],    # read-only: NetworkManager then does without udev
                 ["-t", "tmpfs", "tmpfs", "/run"],
                 ["-t", "tmpfs", "tmpfs", "/var/lib/NetworkManager"],
                 ["-t", "tmpfs", "tmpfs", "/etc/NetworkManager"],
                 # the WiFi forget's copy on DATA (/mnt/backups) and what S04restore_backup_config puts in it
                 ["-t", "tmpfs", "tmpfs", "/mnt"],
                 ["-t", "tmpfs", "tmpfs", "/etc/ssh"],
                 ["-t", "tmpfs", "tmpfs", "/root"]):
        subprocess.run([tool("mount"), *args], check=True)
    work = tempfile.mkdtemp(prefix="x6100-wifi-restore.")
    for d in ("/run/dbus", "/run/NetworkManager", settings.WIFI_DIR):
        os.makedirs(d, exist_ok=True)
    with open("/etc/NetworkManager/NetworkManager.conf", "w") as f:
        f.write(NM_CONF)
    with open(os.path.join(work, "bus.conf"), "w") as f:
        f.write(BUS_CONF)
    log = open(os.path.join(work, "nm.log"), "w")
    procs.append(subprocess.Popen([tool("dbus-daemon"), "--config-file=" + os.path.join(work, "bus.conf"), "--nofork",
                                   "--nopidfile"]))
    for _ in range(50):
        if os.path.exists(BUS):
            break
        time.sleep(0.1)
    procs.append(subprocess.Popen([tool("NetworkManager"), "--no-daemon",
                                   "--config=/etc/NetworkManager/NetworkManager.conf"], stdout=log, stderr=log))
    log.close()
    for _ in range(100):
        if nm("-t", "general", "status").returncode == 0:
            break
        time.sleep(0.2)
    else:
        sys.exit("NetworkManager did not come up: " + open(os.path.join(work, "nm.log")).read()[-2000:])
    os.environ["PATH"] = radio_bin(work)
    assert shutil.which("dbus-send"), "the radio images have dbus-send, this PC not?"
    assert not shutil.which("nmcli"), "a radio image with nmcli: the test is not as on the radio"


def tearDownModule():
    for p in reversed(procs):
        p.terminate()
        p.wait(10)
    if work and os.environ.get("KEEP"):
        print("NetworkManager's log:", os.path.join(work, "nm.log"))
    elif work:
        shutil.rmtree(work, ignore_errors=True)


def profile(name, ssid, uid, psk="a-password-1", used=0):
    """A saved WiFi network as NetworkManager writes it to system-connections; used: when it was last joined"""
    stamp = f"timestamp={used}\n" if used else ""
    return (f"[connection]\nid={name}\nuuid={uid}\ntype=wifi\n{stamp}\n[wifi]\nmode=infrastructure\nssid={ssid}\n\n"
            f"[wifi-security]\nkey-mgmt=wpa-psk\npsk={psk}\n\n"
            "[ipv4]\nmethod=auto\n\n[ipv6]\nmethod=ignore\n").encode()


def wired(uid, addr):
    """The Ethernet profile with a static address, on a dummy link: there is no cable here"""
    return (f"[connection]\nid=Ethernet\nuuid={uid}\ntype=dummy\ninterface-name=dum0\n\n[dummy]\n\n"
            f"[ipv4]\nmethod=manual\naddress1={addr}/24\n\n[ipv6]\nmethod=disabled\n").encode()


def backup(members):
    """A backup file as the Backup page downloads it: manifest.json and the members, gzipped"""
    buf = io.BytesIO()
    manifest = json.dumps({"version": 1, "date": "2026-11-04 12:00", "callsign": "N0CALL"}).encode()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for name, data in [("manifest.json", manifest)] + sorted(members.items()):
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), 0o600
            tf.addfile(info, io.BytesIO(data))
    return buf.getvalue()


class WifiRestore(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="x6100-wifi-restore.", dir=work)
        for name in ("BACKUP_WORK", "BACKUP_SAFETY"):
            self.addCleanup(setattr, settings, name, getattr(settings, name))
        settings.BACKUP_WORK = os.path.join(self.root, "backup_work")
        settings.BACKUP_SAFETY = os.path.join(self.root, "backup_before_restore")
        os.mkdir(settings.BACKUP_WORK)
        self.addCleanup(os.environ.pop, "DBUS_SYSTEM_BUS_ADDRESS", None)
        for n in os.listdir(settings.WIFI_DIR):     # each case on a radio with no WiFi networks
            os.remove(os.path.join(settings.WIFI_DIR, n))
        nm("connection", "reload")
        self.assertEqual(known(), {})
        self.assertIsNone(until(link, None))

    def have(self, name, data):
        """A network the radio has: its file, and NetworkManager knows it"""
        with open(os.path.join(settings.WIFI_DIR, name), "wb") as f:
            f.write(data)
        os.chmod(os.path.join(settings.WIFI_DIR, name), 0o600)
        nm("connection", "reload")

    def req(self, method, path, data=b"", ctype="application/octet-stream"):
        env = {"REQUEST_METHOD": method, "PATH_INFO": path, "HTTP_HOST": "192.168.1.10", "CONTENT_TYPE": ctype,
               "CONTENT_LENGTH": str(len(data)), "wsgi.input": io.BytesIO(data)}
        setup_testing_defaults(env)
        got = {}

        def start(status, hdrs, exc=None):
            got["status"] = int(status.split()[0])

        out = b"".join(apps.app(env, start))
        self.assertEqual(got["status"], 200, out)
        return json.loads(out)

    def restore(self, members, parts=("wifi",)):
        """Upload a backup and restore these parts of it, as the Backup page does; the restore's answer"""
        up = self.req("POST", "/api/backup/upload", backup(members))
        body = json.dumps({"parts": list(parts), "token": up["token"]}).encode()
        return self.req("POST", "/api/backup/restore", body, "application/json")

    def test_new_network(self):
        r = self.restore({"wifi/Home.nmconnection": profile("Home", "HomeNet", UUID_HOME),
                          "wifi/Club.nmconnection": profile("Club", "ClubNet", UUID_CLUB)})
        self.assertEqual((r["restored"], r["failed"], r["gui_restarted"]), (["wifi"], {}, False))
        reloaded()
        self.assertEqual(known(), {"Home": "HomeNet", "Club": "ClubNet"})
        self.assertEqual(os.stat(os.path.join(settings.WIFI_DIR, "Home.nmconnection")).st_mode & 0o777, 0o600)

    def test_changed_network(self):
        self.have("Home.nmconnection", profile("Home", "OldNet", UUID_HOME))
        self.assertEqual(known(), {"Home": "OldNet"})
        r = self.restore({"wifi/Home.nmconnection": profile("Home", "HomeNet", UUID_HOME)})
        self.assertEqual((r["restored"], r["failed"]), (["wifi"], {}))
        reloaded()
        self.assertEqual(known(), {"Home": "HomeNet"})
        with open(os.path.join(settings.BACKUP_SAFETY, "wifi", "Home.nmconnection"), "rb") as f:
            self.assertIn(b"ssid=OldNet", f.read())    # the radio's own, kept

    def test_same_network_other_name(self):
        self.have("HomeNet.nmconnection", profile("HomeNet", "HomeNet", UUID_HOME, "old-password"))
        self.assertEqual(known(), {"HomeNet": "HomeNet"})
        r = self.restore({"wifi/Home.nmconnection": profile("Home", "HomeNet", UUID_CLUB, "new-password", 1762000200),
                          "wifi/Home 2.nmconnection": profile("Home 2", "HomeNet", UUID_ETH, "older-password",
                                                              1762000100)})
        self.assertEqual((r["restored"], r["failed"]), (["wifi"], {}))
        reloaded()
        self.assertEqual(sorted(os.listdir(settings.WIFI_DIR)), ["HomeNet.nmconnection"])   # one profile of it
        self.assertEqual(known(), {"Home": "HomeNet"})      # the backup's latest used
        with open(os.path.join(settings.WIFI_DIR, "HomeNet.nmconnection"), "rb") as f:
            self.assertIn(b"psk=new-password", f.read())
        with open(os.path.join(settings.BACKUP_SAFETY, "wifi", "HomeNet.nmconnection"), "rb") as f:
            self.assertIn(b"psk=old-password", f.read())    # the radio's own, kept

    def test_networkmanager_not_reachable(self):
        os.environ["DBUS_SYSTEM_BUS_ADDRESS"] = "unix:path=/run/no-bus-here"
        r = self.restore({"wifi/Home.nmconnection": profile("Home", "HomeNet", UUID_HOME)})
        self.assertEqual((r["restored"], r["failed"]), (["wifi"], {}))
        reloaded()      # with no bus to reach
        self.assertTrue(os.path.isfile(os.path.join(settings.WIFI_DIR, "Home.nmconnection")))
        self.assertEqual(known(), {})
        nm("connection", "reload")      # as NetworkManager's next start reads it
        self.assertEqual(known(), {"Home": "HomeNet"})

    def test_network_in_use_replaced(self):
        self.have("Ethernet.nmconnection", wired(UUID_ETH, "10.9.0.2"))
        self.assertEqual(until(link, (UUID_ETH, "10.9.0.2/24")), (UUID_ETH, "10.9.0.2/24"))
        r = self.restore({"wifi/Ethernet.nmconnection": wired(UUID_ETH_OTHER, "10.9.0.3")})
        self.assertEqual(link(), (UUID_ETH, "10.9.0.2/24"))     # the answer went out on the link it came in on
        self.assertEqual((r["restored"], r["failed"]), (["wifi"], {}))
        reloaded()
        self.assertEqual(until(link, (UUID_ETH_OTHER, "10.9.0.3/24")), (UUID_ETH_OTHER, "10.9.0.3/24"))


FORGET = os.path.join(TOP, "rootfs", "usr", "sbin", "x6100-wifi-forget")
BACKUP_CONFIG = os.path.join(TOP, "rootfs", "etc", "init.d", "S04restore_backup_config")
COPY = "/mnt/backups/backup.tgz"
UUID_TETHER = "441863a7-da0e-48a6-8f9f-4823484d6da7"


def tether():
    """The image's USB tethering profile: Ethernet, matched by driver"""
    return (f"[connection]\nid=USB tethering\nuuid={UUID_TETHER}\ntype=ethernet\nautoconnect-priority=50\n\n"
            "[match]\ndriver=rndis_host;\n\n[ethernet]\n\n[ipv4]\nmethod=auto\nnever-default=true\n").encode()


def older(name, ssid, uid):
    """A WiFi profile as older NetworkManagers wrote it: type=802-11-wireless"""
    return (f"[connection]\nid={name}\nuuid={uid}\ntype=802-11-wireless\n\n[802-11-wireless]\nmode=infrastructure\n"
            f"ssid={ssid}\n\n[ipv4]\nmethod=auto\n").encode()


class WifiForget(unittest.TestCase):
    def setUp(self):
        self.addCleanup(os.environ.pop, "DBUS_SYSTEM_BUS_ADDRESS", None)
        for n in os.listdir(settings.WIFI_DIR):
            os.remove(os.path.join(settings.WIFI_DIR, n))
        nm("connection", "reload")
        for d in ("/etc/ssh", "/root/.ssh", "/mnt/backups"):
            os.makedirs(d, exist_ok=True)
        with open("/etc/ssh/ssh_host_ed25519_key", "w") as f:
            f.write("a host key\n")
        if os.path.exists(COPY):
            os.remove(COPY)

    def have(self, name, data):
        with open(os.path.join(settings.WIFI_DIR, name), "wb") as f:
            f.write(data)
        os.chmod(os.path.join(settings.WIFI_DIR, name), 0o600)

    def run_sh(self, *args, env=None):
        return subprocess.run(["sh", *args], capture_output=True, text=True, timeout=60,
                              env={**os.environ, **(env or {})})

    def copy_made(self):
        """The copy on DATA, as at a clean power-off"""
        self.assertEqual(self.run_sh(BACKUP_CONFIG, "stop").returncode, 0)
        return self.members()

    def members(self):
        with tarfile.open(COPY) as tf:
            return sorted(tf.getnames())

    def forget(self, **env):
        r = self.run_sh(FORGET, env={"X6100_BACKUP_CONFIG": BACKUP_CONFIG, **env})
        return r.returncode, r.stdout.strip()

    def test_wifi_gone_others_stay(self):
        self.have("Home.nmconnection", profile("Home", "HomeNet", UUID_HOME))
        self.have("Cafe [5G].nmconnection", profile("Cafe [5G]", "Cafe 5G", UUID_CLUB))
        self.have("Old", older("Old", "OldNet", UUID_ETH_OTHER))
        self.have("Ethernet.nmconnection", wired(UUID_ETH, "10.9.0.2"))
        self.have("usb-tether.nmconnection", tether())
        nm("connection", "reload")
        self.assertEqual(known(), {"Home": "HomeNet", "Cafe [5G]": "Cafe 5G", "Old": "OldNet"})
        before = self.copy_made()
        self.assertIn("etc/NetworkManager/system-connections/Cafe [5G].nmconnection", before)
        self.assertEqual(self.forget(), (0, "3"))
        self.assertEqual(sorted(os.listdir(settings.WIFI_DIR)), ["Ethernet.nmconnection", "usb-tether.nmconnection"])
        self.assertEqual(until(known, {}), {})          # dropped at once, no restart
        self.assertEqual(self.members(), ["etc/NetworkManager/system-connections/Ethernet.nmconnection",
                                          "etc/NetworkManager/system-connections/usb-tether.nmconnection",
                                          "etc/ssh/ssh_host_ed25519_key"])

    def test_no_wifi(self):
        self.have("Ethernet.nmconnection", wired(UUID_ETH, "10.9.0.2"))
        nm("connection", "reload")
        self.copy_made()
        with open(COPY, "rb") as f:
            copy = f.read()
        self.assertEqual(self.forget(), (0, "0"))
        self.assertEqual(os.listdir(settings.WIFI_DIR), ["Ethernet.nmconnection"])
        with open(COPY, "rb") as f:
            self.assertEqual(f.read(), copy)            # not made again

    def test_networkmanager_not_reachable(self):
        self.have("Home.nmconnection", profile("Home", "HomeNet", UUID_HOME))
        nm("connection", "reload")
        self.copy_made()
        self.assertEqual(self.forget(DBUS_SYSTEM_BUS_ADDRESS="unix:path=/run/no-bus-here"), (0, "1"))
        self.assertEqual(os.listdir(settings.WIFI_DIR), [])
        self.assertEqual(self.members(), ["etc/ssh/ssh_host_ed25519_key"])
        self.assertEqual(known(), {"Home": "HomeNet"})  # NetworkManager still has it...
        nm("connection", "reload")                      # ...until its next start reads the folder
        self.assertEqual(known(), {})

    def test_copy_not_made_again(self):
        self.have("Home.nmconnection", profile("Home", "HomeNet", UUID_HOME))
        nm("connection", "reload")
        self.copy_made()
        self.assertEqual(self.forget(X6100_BACKUP_CONFIG=tool("false")), (0, "1"))
        self.assertFalse(os.path.exists(COPY))          # it named the network: gone, rather than bring it back


if __name__ == "__main__":
    unittest.main()
